package proxy

import (
	"context"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/huangzheng2016/eego-a4/link/internal/proto"
)

func TestPolicy(t *testing.T) {
	p := Policy{Allow: []string{"*.example.com", "api.test"}}
	for host, want := range map[string]bool{"a.example.com": true, "API.TEST": true, "example.com": false, "evil.com": false} {
		if p.AllowsHost(host) != want {
			t.Errorf("%s: want %v", host, want)
		}
	}
	if !(Policy{}).AllowsHost("anything") {
		t.Error("empty allow list must allow all")
	}
	dp := Policy{DenyPrivate: true}
	for ip, want := range map[string]bool{"127.0.0.1": false, "192.168.1.5": false, "10.0.0.1": false, "8.8.8.8": true, "::1": false} {
		if dp.AllowsIP(net.ParseIP(ip)) != want {
			t.Errorf("%s: want %v", ip, want)
		}
	}
}

func server(t *testing.T) *httptest.Server {
	mux := http.NewServeMux()
	mux.HandleFunc("/ok", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer x" || !strings.HasPrefix(r.UserAgent(), "eego-link/") {
			w.WriteHeader(401)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("ETag", `"v1"`)
		w.Header().Set("X-Secret", "dropped")
		_, _ = w.Write([]byte(`{"metrics":[]}`))
	})
	mux.HandleFunc("/nm", func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(http.StatusNotModified) })
	mux.HandleFunc("/big", func(w http.ResponseWriter, r *http.Request) { _, _ = w.Write(make([]byte, 2048)) })
	mux.HandleFunc("/slow", func(w http.ResponseWriter, r *http.Request) { time.Sleep(500 * time.Millisecond) })
	mux.HandleFunc("/loop", func(w http.ResponseWriter, r *http.Request) { http.Redirect(w, r, "/loop", http.StatusFound) })
	s := httptest.NewServer(mux)
	t.Cleanup(s.Close)
	return s
}

func TestExecutor(t *testing.T) {
	s := server(t)
	e := NewExecutor(Policy{}, "eego-link/test")
	ctx := context.Background()

	r := e.Do(ctx, proto.ProxyRequest{M: "GET", U: s.URL + "/ok", H: map[string]string{"authorization": "Bearer x"}}, nil)
	if r.Head.S != 200 || string(r.Body) != `{"metrics":[]}` || r.Head.H["etag"] != `"v1"` || r.Head.H["x-secret"] != "" {
		t.Fatalf("ok: %+v %q", r.Head, r.Body)
	}
	if r := e.Do(ctx, proto.ProxyRequest{M: "GET", U: s.URL + "/nm"}, nil); r.Head.S != 304 || len(r.Body) != 0 {
		t.Errorf("304: %+v", r.Head)
	}
	if r := e.Do(ctx, proto.ProxyRequest{M: "GET", U: s.URL + "/big", Max: 1024}, nil); r.Head.E != "too_large" {
		t.Errorf("too_large: %+v", r.Head)
	}
	if r := e.Do(ctx, proto.ProxyRequest{M: "GET", U: s.URL + "/slow", T: 100}, nil); r.Head.E != "timeout" {
		t.Errorf("timeout: %+v", r.Head)
	}
	if r := e.Do(ctx, proto.ProxyRequest{M: "GET", U: s.URL + "/loop"}, nil); r.Head.S != 0 {
		t.Errorf("redirect cap: %+v", r.Head)
	}
	if r := e.Do(ctx, proto.ProxyRequest{M: "GET", U: "ftp://x/"}, nil); r.Head.E != "bad_request" {
		t.Errorf("bad url: %+v", r.Head)
	}
	if r := e.Do(ctx, proto.ProxyRequest{M: "PATCH", U: s.URL}, nil); r.Head.E != "bad_request" {
		t.Errorf("bad method: %+v", r.Head)
	}
	denied := NewExecutor(Policy{DenyPrivate: true}, "x")
	if r := denied.Do(ctx, proto.ProxyRequest{M: "GET", U: s.URL + "/ok"}, nil); r.Head.E != "denied" {
		t.Errorf("deny private: %+v", r.Head)
	}
	allow := NewExecutor(Policy{Allow: []string{"example.org"}}, "x")
	if r := allow.Do(ctx, proto.ProxyRequest{M: "GET", U: s.URL + "/ok"}, nil); r.Head.E != "denied" {
		t.Errorf("allow list: %+v", r.Head)
	}
}
