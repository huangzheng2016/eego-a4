package proto

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"testing"
)

type fixture struct {
	CRC32 []struct {
		InputHex string `json:"input_hex"`
		CRC      string `json:"crc"`
	} `json:"crc32"`
	Messages []struct {
		Name    string   `json:"name"`
		ID      uint8    `json:"id"`
		Chunk   int      `json:"chunk"`
		Head    string   `json:"head"`
		BodyHex string   `json:"body_hex"`
		Frames  []string `json:"frames"`
	} `json:"messages"`
	Control []struct {
		Name  string `json:"name"`
		Frame string `json:"frame"`
	} `json:"control"`
}

func loadFixture(t *testing.T) fixture {
	t.Helper()
	raw, err := os.ReadFile(filepath.Join("..", "..", "..", "firmware", "test", "host", "fixtures", "ble_tunnel_vectors.json"))
	if err != nil {
		t.Fatalf("fixture: %v", err)
	}
	var f fixture
	if err := json.Unmarshal(raw, &f); err != nil {
		t.Fatal(err)
	}
	return f
}

func mustHex(t *testing.T, s string) []byte {
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatal(err)
	}
	return b
}

func TestCRCVectors(t *testing.T) {
	for _, v := range loadFixture(t).CRC32 {
		got := fmt.Sprintf("%08x", CRC32(mustHex(t, v.InputHex)))
		if got != v.CRC {
			t.Errorf("crc(%s) = %s want %s", v.InputHex, got, v.CRC)
		}
	}
}

func TestMessageVectors(t *testing.T) {
	for _, m := range loadFixture(t).Messages {
		body := mustHex(t, m.BodyHex)
		frames, err := Encode(TypeData, m.ID, []byte(m.Head), body, m.Chunk)
		if err != nil {
			t.Fatal(err)
		}
		if len(frames) != len(m.Frames) {
			t.Fatalf("%s: %d frames want %d", m.Name, len(frames), len(m.Frames))
		}
		r := NewReassembler(TypeData, 1<<20)
		for i, f := range frames {
			if hex.EncodeToString(f) != m.Frames[i] {
				t.Errorf("%s frame %d mismatch", m.Name, i)
			}
			res := r.Feed(mustHex(t, m.Frames[i]), 0)
			if want := OK; i == len(frames)-1 {
				if res != Complete {
					t.Fatalf("%s: last frame %v", m.Name, res)
				}
			} else if res != want {
				t.Fatalf("%s: frame %d %v", m.Name, i, res)
			}
		}
		head, gotBody := r.Message()
		if string(head) != m.Head || !bytes.Equal(gotBody, body) {
			t.Errorf("%s: round trip mismatch", m.Name)
		}
	}
}

func TestControlVectors(t *testing.T) {
	want := map[string][]byte{
		"ack_id2_seq8":  Ack(2, 8),
		"abort_id5_crc": Abort(5, AbortCRC),
		"event_sleep":   Event([]byte(`{"ev":"sleep","in_s":3}`)),
	}
	for _, c := range loadFixture(t).Control {
		if w, ok := want[c.Name]; ok && hex.EncodeToString(w) != c.Frame {
			t.Errorf("%s: %x want %s", c.Name, w, c.Frame)
		}
	}
}

func TestProxyFramesAndAcks(t *testing.T) {
	body := bytes.Repeat([]byte{0xab}, 5000)
	frames, err := Encode(TypeProxy, 200, []byte(`{"s":200}`), body, 244)
	if err != nil {
		t.Fatal(err)
	}
	if frames[0][0]>>4 != TypeProxy || frames[0][1] != 200 {
		t.Fatalf("bad proxy header %x", frames[0][:4])
	}
	r := NewReassembler(TypeProxy, 1<<16)
	acks := 0
	for i, f := range frames {
		res := r.Feed(f, int64(i))
		if _, due := r.AckDue(); due {
			acks++
		}
		if i == len(frames)-1 && res != Complete {
			t.Fatalf("got %v", res)
		}
	}
	want := 0
	for i := range frames {
		if (i+1)%AckWindow == 0 || i == len(frames)-1 {
			want++
		}
	}
	if acks != want {
		t.Errorf("acks=%d frames=%d", acks, len(frames))
	}
	if r2 := NewReassembler(TypeData, 1<<16); r2.Feed(frames[0], 0) != BadFrame {
		t.Error("data reassembler must reject proxy frames")
	}
}

func TestReassemblerErrors(t *testing.T) {
	frames, _ := Encode(TypeData, 1, []byte(`{}`), bytes.Repeat([]byte{1}, 100), 20)
	r := NewReassembler(TypeData, 10)
	if r.Feed(frames[0], 0) != TooLarge {
		t.Error("expected too_large")
	}
	r = NewReassembler(TypeData, 1000)
	r.Feed(frames[0], 0)
	if r.Feed(frames[2], 0) != SeqGap {
		t.Error("expected seq_gap")
	}
	r.Feed(frames[0], 0)
	if r.Feed(frames[1], IdleTimeoutMs+1) != Timeout {
		t.Error("expected timeout")
	}
	bad := append([][]byte{}, frames...)
	last := append([]byte{}, bad[len(bad)-1]...)
	last[len(last)-1] ^= 0xff
	bad[len(bad)-1] = last
	r.Reset()
	var res Result
	for _, f := range bad {
		res = r.Feed(f, 0)
	}
	if res != BadCRC {
		t.Errorf("expected crc, got %v", res)
	}
}

func TestChunk(t *testing.T) {
	if Chunk(0) != 20 || Chunk(256) != 253 || Chunk(517) != 512 {
		t.Error("chunk sizing")
	}
}
