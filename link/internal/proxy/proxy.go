// Package proxy performs the device's HTTP requests on the computer (TLS included) and
// maps failures to the short error codes the firmware understands.
package proxy

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"path"
	"strings"
	"syscall"
	"time"

	"github.com/huangzheng2016/eego-a4/link/internal/proto"
)

const (
	defaultTimeout = 20 * time.Second
	maxTimeout     = 120 * time.Second
	defaultMax     = 512 * 1024
	hardMax        = 4 * 1024 * 1024
	maxRedirects   = 5
)

// Policy decides which destinations the device may reach. The zero value allows all.
type Policy struct {
	Allow       []string // host globs (path.Match), empty = any host
	DenyPrivate bool     // refuse loopback, private, link-local and unspecified addresses
}

// AllowsHost applies the Allow list to a host name (port stripped, case-insensitive).
func (p Policy) AllowsHost(host string) bool {
	if len(p.Allow) == 0 {
		return true
	}
	host = strings.ToLower(strings.TrimSuffix(host, "."))
	for _, g := range p.Allow {
		if ok, _ := path.Match(strings.ToLower(g), host); ok {
			return true
		}
	}
	return false
}

// AllowsIP applies DenyPrivate to a resolved address.
func (p Policy) AllowsIP(ip net.IP) bool {
	if !p.DenyPrivate {
		return true
	}
	return !(ip.IsLoopback() || ip.IsPrivate() || ip.IsLinkLocalUnicast() ||
		ip.IsLinkLocalMulticast() || ip.IsUnspecified() || ip.IsMulticast())
}

var errDenied = errors.New("destination not allowed")

// Executor runs proxy requests.
type Executor struct {
	Policy    Policy
	UserAgent string
	client    *http.Client
}

// NewExecutor builds an HTTP/1.1 client that enforces the policy at dial time, so DNS
// answers pointing at private addresses are caught too.
func NewExecutor(p Policy, userAgent string) *Executor {
	e := &Executor{Policy: p, UserAgent: userAgent}
	dialer := &net.Dialer{
		Timeout: 15 * time.Second,
		Control: func(_, address string, _ syscall.RawConn) error {
			host, _, err := net.SplitHostPort(address)
			if err != nil {
				return err
			}
			if ip := net.ParseIP(host); ip != nil && !p.AllowsIP(ip) {
				return errDenied
			}
			return nil
		},
	}
	transport := &http.Transport{
		Proxy:                 http.ProxyFromEnvironment,
		DialContext:           dialer.DialContext,
		TLSHandshakeTimeout:   15 * time.Second,
		ResponseHeaderTimeout: maxTimeout,
		MaxIdleConnsPerHost:   2,
		IdleConnTimeout:       60 * time.Second,
		TLSNextProto:          map[string]func(string, *tls.Conn) http.RoundTripper{}, // HTTP/1.1 only
	}
	e.client = &http.Client{
		Transport: transport,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			if len(via) >= maxRedirects {
				return fmt.Errorf("stopped after %d redirects", maxRedirects)
			}
			if !p.AllowsHost(req.URL.Hostname()) {
				return errDenied
			}
			return nil
		},
	}
	return e
}

var hopByHop = map[string]bool{
	"host": true, "connection": true, "content-length": true, "transfer-encoding": true,
	"keep-alive": true, "proxy-connection": true, "upgrade": true, "te": true, "trailer": true,
}

var keepResponse = []string{"content-type", "etag", "last-modified", "cache-control"}

// Result is what the agent sends back, plus stats for logging.
type Result struct {
	Head     proto.ProxyResponse
	Body     []byte
	Host     string
	Duration time.Duration
}

func fail(code, msg string) proto.ProxyResponse {
	if len(msg) > 120 {
		msg = msg[:120]
	}
	return proto.ProxyResponse{S: 0, E: code, Msg: msg}
}

// Do executes one request; it never returns an error, failures are encoded in Head.
func (e *Executor) Do(ctx context.Context, req proto.ProxyRequest, body []byte) Result {
	start := time.Now()
	res := Result{}
	defer func() { res.Duration = time.Since(start) }()

	u, err := url.Parse(req.U)
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
		res.Head = fail("bad_request", "invalid url")
		return res
	}
	res.Host = u.Hostname()
	method := strings.ToUpper(req.M)
	switch method {
	case "GET", "POST", "HEAD", "PUT", "DELETE":
	default:
		res.Head = fail("bad_request", "unsupported method")
		return res
	}
	if !e.Policy.AllowsHost(u.Hostname()) {
		res.Head = fail("denied", "host not in allow list")
		return res
	}
	timeout := defaultTimeout
	if req.T > 0 {
		timeout = min(time.Duration(req.T)*time.Millisecond, maxTimeout)
	}
	limit := req.Max
	if limit <= 0 {
		limit = defaultMax
	}
	limit = min(limit, hardMax)

	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	var rd io.Reader
	if len(body) > 0 {
		rd = bytes.NewReader(body)
	}
	hreq, err := http.NewRequestWithContext(ctx, method, u.String(), rd)
	if err != nil {
		res.Head = fail("bad_request", err.Error())
		return res
	}
	for k, v := range req.H {
		if !hopByHop[strings.ToLower(k)] {
			hreq.Header.Set(k, v)
		}
	}
	if hreq.Header.Get("User-Agent") == "" {
		hreq.Header.Set("User-Agent", e.UserAgent)
	}
	resp, err := e.client.Do(hreq)
	if err != nil {
		res.Head = classify(ctx, err)
		return res
	}
	defer resp.Body.Close()

	h := map[string]string{}
	for _, k := range keepResponse {
		if v := resp.Header.Get(k); v != "" {
			h[k] = v
		}
	}
	if resp.StatusCode == http.StatusNotModified || method == "HEAD" {
		res.Head = proto.ProxyResponse{S: resp.StatusCode, H: h}
		return res
	}
	data, err := io.ReadAll(io.LimitReader(resp.Body, int64(limit)+1))
	if err != nil {
		res.Head = classify(ctx, err)
		return res
	}
	if len(data) > limit {
		res.Head = fail("too_large", fmt.Sprintf("response exceeds %d bytes", limit))
		return res
	}
	res.Head = proto.ProxyResponse{S: resp.StatusCode, H: h}
	res.Body = data
	return res
}

func classify(ctx context.Context, err error) proto.ProxyResponse {
	var dnsErr *net.DNSError
	var certErr *tls.CertificateVerificationError
	var unknownAuth x509.UnknownAuthorityError
	var hostErr x509.HostnameError
	var invalid x509.CertificateInvalidError
	var recordErr tls.RecordHeaderError
	var opErr *net.OpError
	switch {
	case errors.Is(err, errDenied):
		return fail("denied", "destination not allowed")
	case errors.Is(err, context.DeadlineExceeded) || errors.Is(ctx.Err(), context.DeadlineExceeded):
		return fail("timeout", "request timed out")
	case errors.As(err, &dnsErr):
		return fail("dns", dnsErr.Err)
	case errors.As(err, &certErr), errors.As(err, &unknownAuth), errors.As(err, &hostErr),
		errors.As(err, &invalid), errors.As(err, &recordErr):
		return fail("tls", "certificate or TLS error")
	case errors.As(err, &opErr) && opErr.Op == "dial":
		return fail("connect", "connection failed")
	case strings.Contains(err.Error(), "redirects"):
		return fail("network", "too many redirects")
	default:
		return fail("network", "request failed")
	}
}
