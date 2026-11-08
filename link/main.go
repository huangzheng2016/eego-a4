// Command eego-link lets an EEGO A4 device use this computer's network over BLE:
// the device sends HTTP(S) requests through the paired link and the agent performs them.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/huangzheng2016/eego-a4/link/internal/blelink"
	"github.com/huangzheng2016/eego-a4/link/internal/config"
	"github.com/huangzheng2016/eego-a4/link/internal/proxy"
	"github.com/huangzheng2016/eego-a4/link/internal/session"
)

var version = "dev"

func usage() {
	fmt.Fprintf(os.Stderr, `eego-link %s — network uplink for EEGO A4 over Bluetooth

Usage:
  eego-link pair [--name EEGO-A4-XXXX]   pair with a device (code shown on its screen)
  eego-link run [flags]                  serve the paired device (default)
  eego-link status                       show the device status over Bluetooth
  eego-link version

Run flags:
  --allow host1,*.example.com   only allow these destination hosts (default: any)
  --deny-private                refuse loopback/LAN/link-local destinations
  -v                            log every proxied request
`, version)
}

func main() {
	log.SetFlags(log.LstdFlags)
	log.SetPrefix("eego-link: ")
	args := os.Args[1:]
	cmd := "run"
	if len(args) > 0 && !strings.HasPrefix(args[0], "-") {
		cmd, args = args[0], args[1:]
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	var err error
	switch cmd {
	case "pair":
		err = pair(ctx, args)
	case "run":
		err = run(ctx, args)
	case "status":
		err = status(ctx)
	case "version":
		fmt.Println(version)
	case "help", "-h", "--help":
		usage()
	default:
		usage()
		os.Exit(2)
	}
	if err != nil && !errors.Is(err, context.Canceled) {
		log.Fatal(err)
	}
}

func readPasskey(string) (uint32, error) {
	fmt.Fprint(os.Stderr, "Enter the 6-digit code shown on the device screen: ")
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	if err != nil {
		return 0, err
	}
	n, err := strconv.ParseUint(strings.TrimSpace(line), 10, 32)
	if err != nil || n > 999999 {
		return 0, errors.New("invalid code")
	}
	return uint32(n), nil
}

func pair(ctx context.Context, args []string) error {
	fs := flag.NewFlagSet("pair", flag.ExitOnError)
	name := fs.String("name", "", "device name (EEGO-A4-XXXX); default: strongest nearby")
	_ = fs.Parse(args)
	unregister, err := blelink.PreparePairing(readPasskey)
	if err != nil {
		return err
	}
	defer unregister()
	log.Printf("scanning for EEGO-A4 devices...")
	var match func(blelink.Found) bool
	if *name != "" {
		match = func(f blelink.Found) bool { return f.Name == *name }
	}
	found, err := blelink.Scan(ctx, 8*time.Second, match)
	if err != nil {
		return err
	}
	if len(found) == 0 {
		return errors.New("no EEGO-A4 device found; make sure Bluetooth management is on (device Settings) and the device is awake")
	}
	sort.Slice(found, func(i, j int) bool { return found[i].RSSI > found[j].RSSI })
	dev := found[0]
	log.Printf("pairing with %s (%s, %d dBm)", dev.Name, dev.Address, dev.RSSI)
	conn, err := blelink.Connect(dev, log.Default())
	if err != nil {
		return err
	}
	defer conn.Close()
	if conn.Info != nil {
		log.Printf("paired: %s, firmware %s", conn.Info.Name, conn.Info.FW)
		if conn.Info.Name != "" {
			dev.Name = conn.Info.Name
		}
	}
	if err := config.Save(config.Config{Name: dev.Name, Address: dev.Address}); err != nil {
		return err
	}
	p, _ := config.Path()
	log.Printf("saved %s; now run: eego-link run", p)
	return nil
}

func connectSaved(ctx context.Context) (*blelink.Conn, error) {
	cfg, err := config.Load()
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, errors.New("no paired device; run: eego-link pair")
		}
		return nil, err
	}
	found, err := blelink.Scan(ctx, 20*time.Second, func(f blelink.Found) bool {
		return (cfg.Name != "" && f.Name == cfg.Name) || (cfg.Address != "" && strings.EqualFold(f.Address, cfg.Address))
	})
	if err != nil {
		return nil, err
	}
	if len(found) != 1 {
		return nil, fmt.Errorf("%s not in range or not advertising", cfg.Name)
	}
	return blelink.Connect(found[0], log.Default())
}

func run(ctx context.Context, args []string) error {
	fs := flag.NewFlagSet("run", flag.ExitOnError)
	allow := fs.String("allow", "", "comma-separated destination host globs (default: any)")
	denyPrivate := fs.Bool("deny-private", false, "refuse loopback, LAN and link-local destinations")
	verbose := fs.Bool("v", false, "log every proxied request")
	_ = fs.Parse(args)
	var policy proxy.Policy
	if *allow != "" {
		for _, h := range strings.Split(*allow, ",") {
			if h = strings.TrimSpace(h); h != "" {
				policy.Allow = append(policy.Allow, h)
			}
		}
	}
	policy.DenyPrivate = *denyPrivate
	exec := proxy.NewExecutor(policy, "eego-link/"+version)
	host, _ := os.Hostname()

	backoff := 2 * time.Second
	for ctx.Err() == nil {
		conn, err := connectSaved(ctx)
		if err != nil {
			log.Printf("%v; retrying in %s", err, backoff)
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(backoff):
			}
			backoff = min(backoff*2, 60*time.Second)
			continue
		}
		backoff = 2 * time.Second
		name := "device"
		if conn.Info != nil {
			name = conn.Info.Name
		}
		log.Printf("connected to %s (frame %d B)", name, conn.Chunk())
		s := session.New(conn, exec, log.Default(), *verbose)
		done := make(chan error, 1)
		go func() { done <- s.Run(ctx) }()
		if err := s.Event(map[string]any{"ev": "agent", "v": 1, "name": host,
			"os": runtime.GOOS + "/" + runtime.GOARCH, "ver": version}); err != nil {
			log.Printf("hello failed: %v", err)
		}
		go syncTime(ctx, s)
		err = <-done
		conn.Close()
		if ctx.Err() != nil {
			return ctx.Err()
		}
		log.Printf("disconnected (%v); reconnecting", err)
	}
	return ctx.Err()
}

func syncTime(ctx context.Context, s *session.Session) {
	body, _ := json.Marshal(map[string]int64{"unix_ms": time.Now().UnixMilli()})
	head, resp, err := s.Do(ctx, "POST", "/device/time", body, 10*time.Second)
	if err != nil {
		log.Printf("time sync failed: %v", err)
		return
	}
	if head.S != 200 {
		log.Printf("time sync: HTTP %d %s", head.S, resp)
		return
	}
	var r struct {
		Applied bool `json:"applied"`
		Drift   int  `json:"drift_s"`
	}
	if json.Unmarshal(resp, &r) == nil && r.Applied {
		log.Printf("device clock set (drift %ds)", r.Drift)
	}
}

func status(ctx context.Context) error {
	conn, err := connectSaved(ctx)
	if err != nil {
		return err
	}
	defer conn.Close()
	s := session.New(conn, proxy.NewExecutor(proxy.Policy{}, "eego-link/"+version), log.Default(), false)
	go func() { _ = s.Run(ctx) }()
	head, body, err := s.Do(ctx, "GET", "/status", nil, 20*time.Second)
	if err != nil {
		return err
	}
	if head.S != 200 {
		return fmt.Errorf("device returned HTTP %d: %s", head.S, body)
	}
	var st struct {
		Device struct {
			FW     string `json:"fw_version"`
			Uptime int64  `json:"uptime_s"`
			Mode   string `json:"mode"`
		} `json:"device"`
		Network struct {
			SSID string `json:"ssid"`
			IP   string `json:"ip"`
			RSSI int    `json:"rssi"`
		} `json:"network"`
	}
	if err := json.Unmarshal(body, &st); err != nil {
		fmt.Println(string(body))
		return nil
	}
	fmt.Printf("firmware  %s\nuptime    %s\nmode      %s\nwifi      %s %s (%d dBm)\n",
		st.Device.FW, time.Duration(st.Device.Uptime)*time.Second, st.Device.Mode,
		orDash(st.Network.SSID), orDash(st.Network.IP), st.Network.RSSI)
	return nil
}

func orDash(s string) string {
	if s == "" {
		return "-"
	}
	return s
}
