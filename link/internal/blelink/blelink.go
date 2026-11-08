// Package blelink connects to the device with tinygo.org/x/bluetooth and exposes the
// tunnel characteristics as a session.Link.
package blelink

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"strings"
	"sync"
	"time"

	"tinygo.org/x/bluetooth"

	"github.com/huangzheng2016/eego-a4/link/internal/proto"
)

var adapter = bluetooth.DefaultAdapter

var (
	enableOnce sync.Once
	enableErr  error

	connMu  sync.Mutex
	current *Conn
)

// Enable powers up the Bluetooth adapter once per process.
func Enable() error {
	enableOnce.Do(func() {
		enableErr = adapter.Enable()
		if enableErr == nil {
			adapter.SetConnectHandler(func(d bluetooth.Device, connected bool) {
				if connected {
					return
				}
				connMu.Lock()
				c := current
				connMu.Unlock()
				if c != nil {
					c.markClosed()
				}
			})
		}
	})
	return enableErr
}

// Found is one scan hit.
type Found struct {
	Name    string
	Address string
	RSSI    int16
	addr    bluetooth.Address
}

func mustUUID(s string) bluetooth.UUID {
	u, err := bluetooth.ParseUUID(s)
	if err != nil {
		panic(err)
	}
	return u
}

var (
	svcUUID  = mustUUID(proto.ServiceUUID)
	infoUUID = mustUUID(proto.InfoUUID)
	rxUUID   = mustUUID(proto.RxUUID)
	txUUID   = mustUUID(proto.TxUUID)
)

// Scan collects EEGO devices for the given duration (or until match returns true).
func Scan(ctx context.Context, d time.Duration, match func(Found) bool) ([]Found, error) {
	if err := Enable(); err != nil {
		return nil, fmt.Errorf("bluetooth adapter: %w", err)
	}
	var mu sync.Mutex
	seen := map[string]Found{}
	var hit *Found
	stop := time.AfterFunc(d, func() { _ = adapter.StopScan() })
	defer stop.Stop()
	go func() {
		<-ctx.Done()
		_ = adapter.StopScan()
	}()
	err := adapter.Scan(func(a *bluetooth.Adapter, r bluetooth.ScanResult) {
		name := r.LocalName()
		if !strings.HasPrefix(name, proto.NamePrefix) && !r.HasServiceUUID(svcUUID) {
			return
		}
		f := Found{Name: name, Address: r.Address.String(), RSSI: r.RSSI, addr: r.Address}
		mu.Lock()
		defer mu.Unlock()
		if prev, ok := seen[f.Address]; ok && f.Name == "" {
			f.Name = prev.Name
		}
		seen[f.Address] = f
		if match != nil && hit == nil && match(f) {
			hit = &f
			_ = a.StopScan()
		}
	})
	if err != nil {
		return nil, err
	}
	mu.Lock()
	defer mu.Unlock()
	if hit != nil {
		return []Found{*hit}, nil
	}
	out := make([]Found, 0, len(seen))
	for _, f := range seen {
		out = append(out, f)
	}
	return out, nil
}

// Conn is a connected, subscribed device.
type Conn struct {
	Info   *proto.Info
	dev    bluetooth.Device
	rx     bluetooth.DeviceCharacteristic
	frames chan []byte
	chunk  int
	once   sync.Once
	done   chan struct{}
	log    *log.Logger
}

// Connect connects to f, subscribes to TX (which triggers OS pairing when needed) and reads
// INFO where the platform supports it.
func Connect(f Found, logger *log.Logger) (*Conn, error) {
	dev, err := adapter.Connect(f.addr, bluetooth.ConnectionParams{ConnectionTimeout: bluetooth.NewDuration(15 * time.Second)})
	if err != nil {
		return nil, fmt.Errorf("connect: %w", err)
	}
	c := &Conn{dev: dev, frames: make(chan []byte, 1024), done: make(chan struct{}), log: logger}
	fail := func(err error) (*Conn, error) {
		_ = dev.Disconnect()
		return nil, err
	}
	if err := afterConnect(dev, f); err != nil {
		return fail(err)
	}
	svcs, err := dev.DiscoverServices([]bluetooth.UUID{svcUUID})
	if err != nil || len(svcs) == 0 {
		return fail(fmt.Errorf("tunnel service not found: %v", err))
	}
	chars, err := svcs[0].DiscoverCharacteristics([]bluetooth.UUID{infoUUID, rxUUID, txUUID})
	if err != nil {
		return fail(fmt.Errorf("characteristics: %w", err))
	}
	var info, rx, tx *bluetooth.DeviceCharacteristic
	for i := range chars {
		switch chars[i].UUID() {
		case infoUUID:
			info = &chars[i]
		case rxUUID:
			rx = &chars[i]
		case txUUID:
			tx = &chars[i]
		}
	}
	if info == nil || rx == nil || tx == nil {
		return fail(errors.New("tunnel characteristics missing"))
	}
	c.rx = *rx
	var subErr error
	for attempt := 1; attempt <= 4; attempt++ {
		// Encrypted characteristic: the first access may race the link encryption.
		if subErr = tx.EnableNotifications(c.onNotify); subErr == nil {
			break
		}
		logger.Printf("enable notifications failed (attempt %d/4): %v", attempt, subErr)
		time.Sleep(time.Duration(attempt) * 500 * time.Millisecond)
	}
	if subErr != nil {
		return fail(fmt.Errorf("subscribe: %w%s", subErr, pairingHint()))
	}
	mtu := 0
	if raw, err := readCharacteristic(*info); err == nil && len(raw) > 0 {
		var in proto.Info
		if json.Unmarshal(raw, &in) == nil {
			c.Info = &in
			mtu = in.MTU
		}
	}
	if mtu == 0 {
		if m, err := tx.GetMTU(); err == nil {
			mtu = int(m)
		}
	}
	c.chunk = proto.Chunk(mtu)
	connMu.Lock()
	current = c
	connMu.Unlock()
	go c.watch()
	return c, nil
}

func (c *Conn) onNotify(buf []byte) {
	f := append([]byte(nil), buf...)
	select {
	case <-c.done:
	case c.frames <- f:
	default:
		c.log.Printf("notification queue full; frame dropped")
	}
}

// watch polls the connection state (connect-handler support differs per platform).
func (c *Conn) watch() {
	t := time.NewTicker(5 * time.Second)
	defer t.Stop()
	for {
		select {
		case <-c.done:
			return
		case <-t.C:
			if ok, err := c.dev.Connected(); err == nil && !ok {
				c.markClosed()
				return
			}
		}
	}
}

func (c *Conn) markClosed() {
	c.once.Do(func() {
		close(c.done)
		close(c.frames)
	})
}

// Write sends one frame to RX.
func (c *Conn) Write(f []byte) error {
	select {
	case <-c.done:
		return errors.New("disconnected")
	default:
	}
	_, err := c.rx.WriteWithoutResponse(f)
	if err != nil {
		_, err = c.rx.Write(f)
	}
	return err
}

// Frames yields TX notifications; closed on disconnect.
func (c *Conn) Frames() <-chan []byte { return c.frames }

// Chunk is the frame size for this link.
func (c *Conn) Chunk() int { return c.chunk }

// Done is closed when the link drops.
func (c *Conn) Done() <-chan struct{} { return c.done }

// Close disconnects.
func (c *Conn) Close() {
	_ = c.dev.Disconnect()
	c.markClosed()
	connMu.Lock()
	if current == c {
		current = nil
	}
	connMu.Unlock()
}
