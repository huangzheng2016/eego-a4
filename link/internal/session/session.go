// Package session runs the agent's side of one BLE link: tunnel requests the agent sends to
// the device (DATA, ids 1..127) and proxy requests the device sends to the agent (PROXY,
// ids 128..255). It only sees frames, so it is tested with an in-memory link.
package session

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"sync"
	"time"

	"github.com/huangzheng2016/eego-a4/link/internal/proto"
	"github.com/huangzheng2016/eego-a4/link/internal/proxy"
)

// Link carries frames: Write goes to the device RX characteristic, Frames yields TX
// notifications and is closed when the link drops.
type Link interface {
	Write(frame []byte) error
	Frames() <-chan []byte
	Chunk() int
}

// Executor performs proxy requests (proxy.Executor in production).
type Executor interface {
	Do(ctx context.Context, req proto.ProxyRequest, body []byte) proxy.Result
}

const (
	maxTunnelResponse = 512 * 1024
	maxProxyRequest   = 256 * 1024
	ackTimeout        = 10 * time.Second
)

var (
	ErrClosed  = errors.New("link closed")
	ErrAborted = errors.New("device aborted the message")
)

type upload struct {
	acked   uint16
	aborted uint16
	wake    chan struct{}
}

type tunnelReply struct {
	head proto.TunnelResponse
	body []byte
	err  error
}

// Session multiplexes one link.
type Session struct {
	link    Link
	exec    Executor
	log     *log.Logger
	verbose bool

	writeMu sync.Mutex

	mu       sync.Mutex
	uploads  map[uint8]*upload
	pending  chan tunnelReply // reply for the in-flight tunnel request
	tunnelID uint8
	closed   bool

	tunnelMu sync.Mutex // one tunnel request at a time
	proxyQ   chan proxyJob
}

type proxyJob struct {
	id   uint8
	req  proto.ProxyRequest
	body []byte
}

// New creates a session; call Run to start processing frames.
func New(link Link, exec Executor, logger *log.Logger, verbose bool) *Session {
	return &Session{link: link, exec: exec, log: logger, verbose: verbose,
		uploads: map[uint8]*upload{}, proxyQ: make(chan proxyJob, 4)}
}

func nowMs() int64 { return time.Now().UnixMilli() }

// Run processes frames until the link closes or ctx ends.
func (s *Session) Run(ctx context.Context) error {
	ctx, cancel := context.WithCancel(ctx)
	defer cancel()
	go s.proxyWorker(ctx)

	dataR := proto.NewReassembler(proto.TypeData, maxTunnelResponse)
	proxyR := proto.NewReassembler(proto.TypeProxy, maxProxyRequest)
	frames := s.link.Frames()
	defer s.shutdown()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case f, ok := <-frames:
			if !ok {
				return ErrClosed
			}
			s.handleFrame(f, dataR, proxyR)
		}
	}
}

func (s *Session) shutdown() {
	s.mu.Lock()
	s.closed = true
	for _, u := range s.uploads {
		close(u.wake)
	}
	s.uploads = map[uint8]*upload{}
	if s.pending != nil {
		s.pending <- tunnelReply{err: ErrClosed}
		s.pending = nil
	}
	s.mu.Unlock()
}

func (s *Session) handleFrame(f []byte, dataR, proxyR *proto.Reassembler) {
	h, ok := proto.ParseHeader(f)
	if !ok {
		return
	}
	switch h.Type {
	case proto.TypeAck, proto.TypeAbort:
		s.mu.Lock()
		if u := s.uploads[h.ID]; u != nil {
			if h.Type == proto.TypeAck && h.Seq > u.acked {
				u.acked = h.Seq
			} else if h.Type == proto.TypeAbort {
				u.aborted = h.Seq
			}
			select {
			case u.wake <- struct{}{}:
			default:
			}
		} else if h.Type == proto.TypeAbort && h.ID == s.tunnelID && s.pending != nil {
			s.pending <- tunnelReply{err: fmt.Errorf("%w (reason %d)", ErrAborted, h.Seq)}
			s.pending = nil
		}
		s.mu.Unlock()
	case proto.TypeEvent:
		if len(f) > proto.HeaderLen {
			s.log.Printf("device event: %s", f[proto.HeaderLen:])
		}
	case proto.TypeData:
		res := dataR.Feed(f, nowMs())
		switch res {
		case proto.OK:
		case proto.Complete:
			head, body := dataR.Message()
			var rh proto.TunnelResponse
			err := json.Unmarshal(head, &rh)
			s.mu.Lock()
			if s.pending != nil && dataR.ID() == int(s.tunnelID) {
				s.pending <- tunnelReply{head: rh, body: append([]byte(nil), body...), err: err}
				s.pending = nil
			}
			s.mu.Unlock()
			dataR.Reset()
		default:
			s.log.Printf("tunnel response dropped: %v", res)
		}
	case proto.TypeProxy:
		res := proxyR.Feed(f, nowMs())
		switch res {
		case proto.OK:
		case proto.Complete:
			head, body := proxyR.Message()
			id := uint8(proxyR.ID())
			var req proto.ProxyRequest
			if err := json.Unmarshal(head, &req); err != nil {
				s.log.Printf("proxy #%d: bad request head", id)
				go s.reply(id, proto.ProxyResponse{E: "bad_request", Msg: "invalid head"}, nil)
			} else {
				select {
				case s.proxyQ <- proxyJob{id: id, req: req, body: append([]byte(nil), body...)}:
				default:
					go s.reply(id, proto.ProxyResponse{E: "network", Msg: "agent busy"}, nil)
				}
			}
			proxyR.Reset()
		default:
			s.log.Printf("proxy request dropped: %v", res)
		}
	}
}

func (s *Session) proxyWorker(ctx context.Context) {
	for {
		select {
		case <-ctx.Done():
			return
		case job := <-s.proxyQ:
			r := s.exec.Do(ctx, job.req, job.body)
			if s.verbose || r.Head.S == 0 {
				status := fmt.Sprint(r.Head.S)
				if r.Head.S == 0 {
					status = r.Head.E
				}
				s.log.Printf("proxy %s %s -> %s %dB %dms", job.req.M, r.Host, status, len(r.Body), r.Duration.Milliseconds())
			}
			if err := s.reply(job.id, r.Head, r.Body); err != nil {
				s.log.Printf("proxy #%d: reply failed: %v", job.id, err)
			}
		}
	}
}

func (s *Session) reply(id uint8, head proto.ProxyResponse, body []byte) error {
	raw, err := json.Marshal(head)
	if err != nil {
		return err
	}
	return s.send(proto.TypeProxy, id, raw, body)
}

func (s *Session) write(f []byte) error {
	s.writeMu.Lock()
	defer s.writeMu.Unlock()
	return s.link.Write(f)
}

// send uploads one message keeping at most AckWindow frames unacknowledged.
func (s *Session) send(typ, id uint8, head, body []byte) error {
	frames, err := proto.Encode(typ, id, head, body, s.link.Chunk())
	if err != nil {
		return err
	}
	u := &upload{wake: make(chan struct{}, 1)}
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return ErrClosed
	}
	s.uploads[id] = u
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		if s.uploads[id] == u {
			delete(s.uploads, id)
		}
		s.mu.Unlock()
	}()
	wait := func(cond func() bool) error {
		deadline := time.NewTimer(ackTimeout)
		defer deadline.Stop()
		for {
			s.mu.Lock()
			done, aborted := cond(), u.aborted
			s.mu.Unlock()
			if aborted != 0 {
				return fmt.Errorf("%w (reason %d)", ErrAborted, aborted)
			}
			if done {
				return nil
			}
			select {
			case _, ok := <-u.wake:
				if !ok {
					return ErrClosed
				}
			case <-deadline.C:
				return errors.New("ack timeout")
			}
		}
	}
	for i, f := range frames {
		seq := uint16(i)
		if err := wait(func() bool { return seq < u.acked+proto.AckWindow }); err != nil {
			return err
		}
		if err := s.write(f); err != nil {
			return err
		}
	}
	total := uint16(len(frames))
	return wait(func() bool { return u.acked >= total })
}

// Event sends a single EVENT frame.
func (s *Session) Event(v any) error {
	raw, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return s.write(proto.Event(raw))
}

// Do sends a tunnel request (path relative to /api/v1) and waits for the device's reply.
func (s *Session) Do(ctx context.Context, method, path string, body []byte, timeout time.Duration) (proto.TunnelResponse, []byte, error) {
	s.tunnelMu.Lock()
	defer s.tunnelMu.Unlock()
	reply := make(chan tunnelReply, 1)
	s.mu.Lock()
	if s.closed {
		s.mu.Unlock()
		return proto.TunnelResponse{}, nil, ErrClosed
	}
	s.tunnelID = s.tunnelID%127 + 1
	id := s.tunnelID
	s.pending = reply
	s.mu.Unlock()

	h := map[string]string{"accept": "application/json"}
	if body != nil {
		h["content-type"] = "application/json"
	}
	head, _ := json.Marshal(proto.TunnelRequest{M: method, P: path, H: h, T: int(timeout.Milliseconds())})
	if err := s.send(proto.TypeData, id, head, body); err != nil {
		s.clearPending(reply)
		return proto.TunnelResponse{}, nil, err
	}
	t := time.NewTimer(timeout + 5*time.Second)
	defer t.Stop()
	select {
	case r := <-reply:
		return r.head, r.body, r.err
	case <-ctx.Done():
		s.clearPending(reply)
		return proto.TunnelResponse{}, nil, ctx.Err()
	case <-t.C:
		s.clearPending(reply)
		return proto.TunnelResponse{}, nil, errors.New("tunnel response timeout")
	}
}

func (s *Session) clearPending(c chan tunnelReply) {
	s.mu.Lock()
	if s.pending == c {
		s.pending = nil
	}
	s.mu.Unlock()
}
