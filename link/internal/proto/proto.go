// Package proto implements the EEGO BLE tunnel framing (protocol v1), byte-identical to
// firmware/components/ble/src/tunnel_frame.cpp and web/src/api/ble/protocol.ts.
//
// Frame (little-endian):
//
//	byte 0   flags: bit0 START, bit1 END, bits4-7 TYPE
//	byte 1   message id
//	byte 2-3 seq: frame index (DATA/PROXY) | next expected seq (ACK) | reason (ABORT)
//	byte 4.. payload
//
// The START frame payload begins with a 10-byte prelude: u32 total_len, u32 crc32 (zlib),
// u16 head_len. Message bytes = head (JSON) + body (raw).
package proto

import (
	"encoding/binary"
	"errors"
	"hash/crc32"
)

const (
	ServiceUUID = "e3e0a400-7c1b-4f7e-9d2a-5ee90a4b0000"
	InfoUUID    = "e3e0a400-7c1b-4f7e-9d2a-5ee90a4b0001"
	RxUUID      = "e3e0a400-7c1b-4f7e-9d2a-5ee90a4b0002"
	TxUUID      = "e3e0a400-7c1b-4f7e-9d2a-5ee90a4b0003"
	NamePrefix  = "EEGO-A4"

	FlagStart = 0x01
	FlagEnd   = 0x02

	TypeData  = 0x0 // central-initiated tunnel request / device response (ids 1..127)
	TypeAck   = 0x1
	TypeAbort = 0x2
	TypeEvent = 0x3
	TypeProxy = 0x4 // device-initiated proxy request / agent response (ids 128..255)

	HeaderLen  = 4
	PreludeLen = 10
	AckWindow  = 8

	AbortBusy      = 1
	AbortTooLarge  = 2
	AbortSeqGap    = 3
	AbortCRC       = 4
	AbortTimeout   = 5
	AbortCancelled = 6

	IdleTimeoutMs = 10000

	FirstProxyID = 128
)

// CRC32 is the IEEE 802.3 / zlib CRC-32.
func CRC32(b []byte) uint32 { return crc32.ChecksumIEEE(b) }

// Header is a parsed frame header.
type Header struct {
	Start, End bool
	Type       uint8
	ID         uint8
	Seq        uint16
}

// ParseHeader returns false for frames shorter than the header.
func ParseHeader(f []byte) (Header, bool) {
	if len(f) < HeaderLen {
		return Header{}, false
	}
	return Header{
		Start: f[0]&FlagStart != 0,
		End:   f[0]&FlagEnd != 0,
		Type:  f[0] >> 4,
		ID:    f[1],
		Seq:   binary.LittleEndian.Uint16(f[2:4]),
	}, true
}

func frame(flags, typ, id uint8, seq uint16, payload []byte) []byte {
	out := make([]byte, HeaderLen+len(payload))
	out[0] = (flags & 0x03) | (typ&0x0f)<<4
	out[1] = id
	binary.LittleEndian.PutUint16(out[2:4], seq)
	copy(out[HeaderLen:], payload)
	return out
}

// Ack acknowledges frames up to (excluding) nextSeq of message id.
func Ack(id uint8, nextSeq uint16) []byte { return frame(0, TypeAck, id, nextSeq, nil) }

// Abort cancels message id with a reason code.
func Abort(id uint8, reason uint16) []byte {
	return frame(FlagStart|FlagEnd, TypeAbort, id, reason, nil)
}

// Event is a single-frame JSON notification (id 0).
func Event(json []byte) []byte { return frame(FlagStart|FlagEnd, TypeEvent, 0, 0, json) }

// ErrChunk is returned when the frame size cannot hold the START prelude.
var ErrChunk = errors.New("proto: chunk too small")

// Encode splits head+body into frames of type typ (TypeData or TypeProxy), each at most
// chunk bytes long.
func Encode(typ, id uint8, head, body []byte, chunk int) ([][]byte, error) {
	if chunk < HeaderLen+PreludeLen+1 {
		return nil, ErrChunk
	}
	if len(head) > 0xffff {
		return nil, errors.New("proto: head too large")
	}
	msg := make([]byte, 0, len(head)+len(body))
	msg = append(msg, head...)
	msg = append(msg, body...)
	prelude := make([]byte, PreludeLen)
	binary.LittleEndian.PutUint32(prelude[0:4], uint32(len(msg)))
	binary.LittleEndian.PutUint32(prelude[4:8], CRC32(msg))
	binary.LittleEndian.PutUint16(prelude[8:10], uint16(len(head)))

	firstCap := chunk - HeaderLen - PreludeLen
	restCap := chunk - HeaderLen
	off := min(firstCap, len(msg))
	first := append(prelude, msg[:off]...)
	flags := uint8(FlagStart)
	if off >= len(msg) {
		flags |= FlagEnd
	}
	frames := [][]byte{frame(flags, typ, id, 0, first)}
	for seq := uint16(1); off < len(msg); seq++ {
		n := min(restCap, len(msg)-off)
		var f uint8
		if off+n >= len(msg) {
			f = FlagEnd
		}
		frames = append(frames, frame(f, typ, id, seq, msg[off:off+n]))
		off += n
	}
	return frames, nil
}

// Result of feeding one frame into a Reassembler.
type Result int

const (
	OK Result = iota
	Complete
	Busy
	TooLarge
	SeqGap
	BadCRC
	Timeout
	BadFrame
)

func (r Result) String() string {
	return [...]string{"ok", "complete", "busy", "too_large", "seq_gap", "crc", "timeout", "bad_frame"}[r]
}

// Reassembler rebuilds one message of a single frame type; same state machine as the
// firmware and web implementations.
type Reassembler struct {
	Type    uint8
	MaxSize int

	buf        []byte
	total      int
	crc        uint32
	headLen    int
	nextSeq    uint16
	id         int
	lastMs     int64
	done       bool
	ackPending bool
}

// NewReassembler accepts frames of type typ with messages up to maxSize bytes.
func NewReassembler(typ uint8, maxSize int) *Reassembler {
	r := &Reassembler{Type: typ, MaxSize: maxSize}
	r.Reset()
	return r
}

func (r *Reassembler) Reset() {
	r.buf, r.total, r.crc, r.headLen, r.nextSeq, r.done, r.ackPending = nil, 0, 0, 0, 0, false, false
	r.id = -1
}

// Active reports a message in progress.
func (r *Reassembler) Active() bool { return r.buf != nil && !r.done }

// ID of the current message (-1 when idle).
func (r *Reassembler) ID() int { return r.id }

// Message returns head and body once Complete.
func (r *Reassembler) Message() (head, body []byte) {
	return r.buf[:r.headLen], r.buf[r.headLen:]
}

// AckDue returns the next expected seq when an ACK should be sent now.
func (r *Reassembler) AckDue() (uint16, bool) {
	if !r.ackPending {
		return 0, false
	}
	r.ackPending = false
	return r.nextSeq, true
}

// Feed consumes one frame received at nowMs.
func (r *Reassembler) Feed(f []byte, nowMs int64) Result {
	h, ok := ParseHeader(f)
	if !ok || h.Type != r.Type {
		return BadFrame
	}
	if r.Active() && nowMs-r.lastMs > IdleTimeoutMs {
		r.Reset()
		if !h.Start {
			return Timeout
		}
	}
	payload := f[HeaderLen:]
	if h.Start {
		if r.Active() {
			return Busy
		}
		if h.Seq != 0 || len(payload) < PreludeLen {
			return BadFrame
		}
		total := int(binary.LittleEndian.Uint32(payload[0:4]))
		headLen := int(binary.LittleEndian.Uint16(payload[8:10]))
		if total > r.MaxSize {
			return TooLarge
		}
		if headLen > total {
			return BadFrame
		}
		r.Reset()
		r.buf = make([]byte, 0, total)
		r.total = total
		r.crc = binary.LittleEndian.Uint32(payload[4:8])
		r.headLen = headLen
		r.id = int(h.ID)
		return r.append(payload[PreludeLen:], h.End, nowMs)
	}
	if !r.Active() || int(h.ID) != r.id {
		return BadFrame
	}
	if h.Seq != r.nextSeq {
		r.Reset()
		return SeqGap
	}
	return r.append(payload, h.End, nowMs)
}

func (r *Reassembler) append(b []byte, end bool, nowMs int64) Result {
	if len(r.buf)+len(b) > r.total {
		r.Reset()
		return BadFrame
	}
	r.buf = append(r.buf, b...)
	r.nextSeq++
	r.lastMs = nowMs
	if r.nextSeq%AckWindow == 0 {
		r.ackPending = true
	}
	if !end {
		return OK
	}
	if len(r.buf) != r.total || CRC32(r.buf) != r.crc {
		r.Reset()
		return BadCRC
	}
	r.done = true
	r.ackPending = true
	return Complete
}
