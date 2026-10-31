package events_test

import (
	"bufio"
	"context"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/events"
)

// fakeTicker hands out tick channels the test drives.
type fakeTicker struct {
	mu    sync.Mutex
	chans []chan time.Time
}

func (f *fakeTicker) new(time.Duration) (<-chan time.Time, func()) {
	f.mu.Lock()
	defer f.mu.Unlock()
	c := make(chan time.Time, 1)
	f.chans = append(f.chans, c)
	return c, func() {}
}

func (f *fakeTicker) tickAll() {
	f.mu.Lock()
	defer f.mu.Unlock()
	for _, c := range f.chans {
		select {
		case c <- time.Now():
		default:
		}
	}
}

type harness struct {
	t      *testing.T
	hub    *events.Hub
	srv    *httptest.Server
	ticker *fakeTicker
}

// newHarness serves a hub where the token "chain-X" is chain X, any other
// token fails, and the client address is the X-Test-IP header.
func newHarness(t *testing.T, tweak func(*events.Config)) *harness {
	ft := &fakeTicker{}
	cfg := events.DefaultConfig()
	cfg.NewTicker = ft.new
	cfg.Authorize = func(r *http.Request) (string, error) {
		tok := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
		if chain, ok := strings.CutPrefix(tok, "chain-"); ok && chain != "" {
			return chain, nil
		}
		return "", errors.New("bad token")
	}
	cfg.ClientIP = func(r *http.Request) string {
		if ip := r.Header.Get("X-Test-IP"); ip != "" {
			return ip
		}
		return "127.0.0.1"
	}
	if tweak != nil {
		tweak(&cfg)
	}
	hub := events.New(cfg)
	srv := httptest.NewServer(hub)
	t.Cleanup(func() {
		hub.Close()
		srv.Close()
	})
	return &harness{t: t, hub: hub, srv: srv, ticker: ft}
}

// stream is one open subscription, read line by line.
type stream struct {
	t      *testing.T
	resp   *http.Response
	lines  chan string
	cancel context.CancelFunc
}

func (h *harness) request(token, client, ip string) (*http.Response, context.CancelFunc) {
	h.t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	url := h.srv.URL + "/"
	if client != "" {
		url += "?client_id=" + client
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	require.NoError(h.t, err)
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	if ip != "" {
		req.Header.Set("X-Test-IP", ip)
	}
	resp, err := http.DefaultClient.Do(req)
	require.NoError(h.t, err)
	return resp, cancel
}

func (h *harness) subscribe(chain, client string) *stream {
	h.t.Helper()
	return h.subscribeFrom(chain, client, "")
}

func (h *harness) subscribeFrom(chain, client, ip string) *stream {
	h.t.Helper()
	resp, cancel := h.request("chain-"+chain, client, ip)
	require.Equal(h.t, http.StatusOK, resp.StatusCode)
	assert.Equal(h.t, "text/event-stream", resp.Header.Get("Content-Type"))
	s := &stream{t: h.t, resp: resp, lines: make(chan string, 100), cancel: cancel}
	go func() {
		defer close(s.lines)
		sc := bufio.NewScanner(resp.Body)
		for sc.Scan() {
			s.lines <- sc.Text()
		}
	}()
	h.t.Cleanup(s.close)
	// The stream announces itself.
	s.expect(": connected", "")
	return s
}

func (s *stream) close() {
	s.cancel()
	s.resp.Body.Close()
}

func (s *stream) next() (string, bool) {
	select {
	case l, ok := <-s.lines:
		return l, ok
	case <-time.After(5 * time.Second):
		s.t.Fatal("no line from the stream in 5 s")
		return "", false
	}
}

func (s *stream) expect(lines ...string) {
	s.t.Helper()
	for _, want := range lines {
		got, ok := s.next()
		require.True(s.t, ok, "stream ended, wanted %q", want)
		require.Equal(s.t, want, got)
	}
}

// quiet checks that nothing arrives for a moment.
func (s *stream) quiet() {
	s.t.Helper()
	select {
	case l, ok := <-s.lines:
		if ok {
			s.t.Fatalf("unexpected line %q", l)
		}
	case <-time.After(100 * time.Millisecond):
	}
}

// ended waits for the server to close the stream.
func (s *stream) ended() {
	s.t.Helper()
	for {
		select {
		case _, ok := <-s.lines:
			if !ok {
				return
			}
		case <-time.After(5 * time.Second):
			s.t.Fatal("stream still open after 5 s")
		}
	}
}

func waitCount(t *testing.T, hub *events.Hub, chain string, n int) {
	t.Helper()
	require.Eventually(t, func() bool { return hub.Count(chain) == n }, 5*time.Second, 5*time.Millisecond)
}

func TestNotifyOtherDevices(t *testing.T) {
	h := newHarness(t, nil)
	a := h.subscribe("AB12", "guid-a")
	b := h.subscribe("ab12", "guid-b") // Same chain, other case.
	other := h.subscribe("ffff", "guid-c")
	waitCount(t, h.hub, "AB12", 2)

	h.hub.Notify("Ab12", "guid-a", []int{970000, 32904})
	b.expect("event: changed", `data: {"types":[32904,970000]}`, "")
	a.quiet()
	other.quiet()

	// Without a committer's GUID, every stream hears it.
	h.hub.Notify("ab12", "", []int{32904})
	a.expect("event: changed", `data: {"types":[32904]}`, "")
	b.expect("event: changed", `data: {"types":[32904]}`, "")
	// No types, no event.
	h.hub.Notify("ab12", "", nil)
	a.quiet()
}

func TestNoClientIDHearsEverything(t *testing.T) {
	h := newHarness(t, nil)
	s := h.subscribe("c1", "")
	waitCount(t, h.hub, "c1", 1)
	h.hub.Notify("c1", "guid-x", []int{1})
	s.expect("event: changed", `data: {"types":[1]}`, "")
}

func TestPendingTypesMerge(t *testing.T) {
	// Notify never blocks; types that arrive before the stream writes go
	// out together, each once.
	h := newHarness(t, nil)
	s := h.subscribe("c1", "b")
	waitCount(t, h.hub, "c1", 1)
	for range 1000 {
		h.hub.Notify("c1", "a", []int{3, 1, 2})
	}
	s.expect("event: changed", `data: {"types":[1,2,3]}`, "")
	for {
		select {
		case l := <-s.lines:
			if l != "" && l != "event: changed" {
				require.Equal(t, `data: {"types":[1,2,3]}`, l)
			}
			continue
		case <-time.After(100 * time.Millisecond):
		}
		break
	}
}

func TestAuth(t *testing.T) {
	h := newHarness(t, func(c *events.Config) {
		c.IsDisabled = func(_ *http.Request, chain string) (bool, error) {
			if chain == "broken" {
				return false, errors.New("db down")
			}
			return chain == "off", nil
		}
	})
	for _, tc := range []struct {
		token string
		code  int
	}{
		{"", http.StatusUnauthorized},
		{"nonsense", http.StatusUnauthorized},
		{"chain-off", http.StatusForbidden},
		{"chain-broken", http.StatusInternalServerError},
	} {
		resp, cancel := h.request(tc.token, "", "")
		assert.Equal(t, tc.code, resp.StatusCode, "token %q", tc.token)
		resp.Body.Close()
		cancel()
	}

	req, err := http.NewRequest(http.MethodPost, h.srv.URL+"/", nil)
	require.NoError(t, err)
	req.Header.Set("Authorization", "Bearer chain-c1")
	resp, err := http.DefaultClient.Do(req)
	require.NoError(t, err)
	resp.Body.Close()
	assert.Equal(t, http.StatusMethodNotAllowed, resp.StatusCode)

	resp, cancel := h.request("chain-c1", strings.Repeat("x", 200), "")
	assert.Equal(t, http.StatusBadRequest, resp.StatusCode)
	resp.Body.Close()
	cancel()
}

func TestLimits(t *testing.T) {
	h := newHarness(t, func(c *events.Config) {
		c.MaxPerChain = 2
		c.MaxPerIP = 3
	})
	s1 := h.subscribeFrom("c1", "1", "10.0.0.1")
	h.subscribeFrom("c1", "2", "10.0.0.1")
	waitCount(t, h.hub, "c1", 2)

	// A third stream for the chain is refused...
	resp, cancel := h.request("chain-c1", "3", "10.0.0.2")
	assert.Equal(t, http.StatusTooManyRequests, resp.StatusCode)
	assert.Equal(t, "60", resp.Header.Get("Retry-After"))
	resp.Body.Close()
	cancel()

	// ...and a fourth from one address, whatever the chain.
	h.subscribeFrom("c2", "1", "10.0.0.1")
	resp, cancel = h.request("chain-c3", "1", "10.0.0.1")
	assert.Equal(t, http.StatusTooManyRequests, resp.StatusCode)
	resp.Body.Close()
	cancel()

	// A stream that goes frees its place.
	s1.close()
	waitCount(t, h.hub, "c1", 1)
	h.subscribeFrom("c1", "3", "10.0.0.2")
	waitCount(t, h.hub, "c1", 2)
	h.subscribeFrom("c3", "1", "10.0.0.1")
}

func TestKeepAlive(t *testing.T) {
	h := newHarness(t, nil)
	s := h.subscribe("c1", "a")
	waitCount(t, h.hub, "c1", 1)
	s.quiet()
	h.ticker.tickAll()
	s.expect(": keep-alive", "")
	h.ticker.tickAll()
	s.expect(": keep-alive", "")
}

func TestKeepAliveRealTicker(t *testing.T) {
	h := newHarness(t, func(c *events.Config) {
		c.KeepAlive = 20 * time.Millisecond
		c.NewTicker = events.DefaultConfig().NewTicker
	})
	s := h.subscribe("c1", "a")
	s.expect(": keep-alive", "", ": keep-alive", "")
}

func TestDisconnectCleansUp(t *testing.T) {
	h := newHarness(t, nil)
	a := h.subscribe("c1", "a")
	h.subscribe("c1", "b")
	waitCount(t, h.hub, "c1", 2)
	a.close()
	waitCount(t, h.hub, "c1", 1)
	// Notifying a chain whose streams are gone is fine.
	h.hub.Notify("c9", "", []int{1})
}

func TestShutdownEndsStreams(t *testing.T) {
	h := newHarness(t, nil)
	a := h.subscribe("c1", "a")
	b := h.subscribe("c2", "b")
	waitCount(t, h.hub, "c1", 1)
	waitCount(t, h.hub, "c2", 1)
	h.hub.Close()
	a.ended()
	b.ended()
	waitCount(t, h.hub, "c1", 0)
	waitCount(t, h.hub, "c2", 0)

	// New streams are refused.
	resp, cancel := h.request("chain-c1", "a", "")
	defer cancel()
	assert.Equal(t, http.StatusServiceUnavailable, resp.StatusCode)
	_, _ = io.Copy(io.Discard, resp.Body)
	resp.Body.Close()
	h.hub.Close() // Twice is fine.
}

func TestHTTPServerShutdown(t *testing.T) {
	// As StartServer wires it: Shutdown ends the streams through
	// RegisterOnShutdown and returns.
	cfg := events.DefaultConfig()
	cfg.Authorize = func(*http.Request) (string, error) { return "c1", nil }
	cfg.ClientIP = func(*http.Request) string { return "ip" }
	hub := events.New(cfg)
	srv := httptest.NewUnstartedServer(hub)
	srv.Config.RegisterOnShutdown(hub.Close)
	srv.Start()
	defer srv.Close()

	resp, err := http.Get(srv.URL + "/")
	require.NoError(t, err)
	defer resp.Body.Close()
	require.Equal(t, http.StatusOK, resp.StatusCode)
	require.Eventually(t, func() bool { return hub.Count("c1") == 1 }, 5*time.Second, 5*time.Millisecond)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	require.NoError(t, srv.Config.Shutdown(ctx))
	_, err = io.ReadAll(resp.Body)
	require.NoError(t, err) // The stream ended cleanly.
	assert.Equal(t, 0, hub.Count("c1"))
}
