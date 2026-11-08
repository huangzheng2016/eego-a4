package session

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log"
	"testing"
	"time"

	"github.com/huangzheng2016/eego-a4/link/internal/proto"
	"github.com/huangzheng2016/eego-a4/link/internal/proxy"
)

// fakeLink connects the session to an in-memory device.
type fakeLink struct {
	toDevice chan []byte
	toAgent  chan []byte
	chunk    int
}

func (l *fakeLink) Write(f []byte) error  { l.toDevice <- append([]byte(nil), f...); return nil }
func (l *fakeLink) Frames() <-chan []byte { return l.toAgent }
func (l *fakeLink) Chunk() int            { return l.chunk }

type stubExec struct{ body []byte }

func (e stubExec) Do(_ context.Context, req proto.ProxyRequest, _ []byte) proxy.Result {
	return proxy.Result{Head: proto.ProxyResponse{S: 200, H: map[string]string{"content-type": "text/plain"}}, Body: e.body, Host: req.U}
}

// device emulates the firmware: answers tunnel requests and ACKs uploads.
func device(t *testing.T, l *fakeLink, proxyReplies chan<- []byte) {
	dataR := proto.NewReassembler(proto.TypeData, 1<<20)
	proxyR := proto.NewReassembler(proto.TypeProxy, 1<<20)
	for f := range l.toDevice {
		h, _ := proto.ParseHeader(f)
		r := dataR
		if h.Type == proto.TypeProxy {
			r = proxyR
		} else if h.Type != proto.TypeData {
			continue
		}
		res := r.Feed(f, time.Now().UnixMilli())
		if seq, due := r.AckDue(); due {
			l.toAgent <- proto.Ack(h.ID, seq)
		}
		if res != proto.Complete {
			continue
		}
		head, body := r.Message()
		if h.Type == proto.TypeData {
			var req proto.TunnelRequest
			_ = json.Unmarshal(head, &req)
			out, _ := json.Marshal(proto.TunnelResponse{S: 200, H: map[string]string{"content-type": "application/json"}})
			frames, _ := proto.Encode(proto.TypeData, h.ID, out, []byte(`{"path":"`+req.P+`","n":`+itoa(len(body))+`}`), l.chunk)
			for _, fr := range frames {
				l.toAgent <- fr
			}
		} else {
			proxyReplies <- append(append([]byte(nil), head...), body...)
		}
		r.Reset()
	}
}

func itoa(n int) string { b, _ := json.Marshal(n); return string(b) }

func TestTunnelAndProxy(t *testing.T) {
	l := &fakeLink{toDevice: make(chan []byte, 256), toAgent: make(chan []byte, 256), chunk: 20}
	big := bytes.Repeat([]byte("x"), 3000) // many frames: exercises the ACK window
	s := New(l, stubExec{body: big}, log.New(io.Discard, "", 0), false)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	go func() { _ = s.Run(ctx) }()
	replies := make(chan []byte, 1)
	go device(t, l, replies)

	head, body, err := s.Do(ctx, "POST", "/device/time", bytes.Repeat([]byte("y"), 500), 5*time.Second)
	if err != nil || head.S != 200 || string(body) != `{"path":"/device/time","n":500}` {
		t.Fatalf("tunnel: %v %+v %s", err, head, body)
	}

	req, _ := json.Marshal(proto.ProxyRequest{M: "GET", U: "https://example.com/x", T: 1000, Max: 4096})
	frames, _ := proto.Encode(proto.TypeProxy, 200, req, nil, l.chunk)
	for _, f := range frames {
		l.toAgent <- f
	}
	select {
	case msg := <-replies:
		if !bytes.HasPrefix(msg, []byte(`{"s":200,"h":{"content-type":"text/plain"}}`)) || !bytes.HasSuffix(msg, big) {
			t.Fatalf("proxy reply %q...", msg[:40])
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no proxy reply")
	}
	if err := s.Event(map[string]any{"ev": "agent", "v": 1}); err != nil {
		t.Fatal(err)
	}
}

func TestClosedLink(t *testing.T) {
	l := &fakeLink{toDevice: make(chan []byte, 16), toAgent: make(chan []byte), chunk: 20}
	s := New(l, stubExec{}, log.New(io.Discard, "", 0), false)
	done := make(chan error)
	go func() { done <- s.Run(context.Background()) }()
	close(l.toAgent)
	if err := <-done; err != ErrClosed {
		t.Fatalf("run: %v", err)
	}
	if _, _, err := s.Do(context.Background(), "GET", "/status", nil, time.Second); err == nil {
		t.Fatal("expected error on closed link")
	}
}
