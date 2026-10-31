package pairing_test

import (
	"encoding/base64"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/pairing"
)

type clock struct {
	mu  sync.Mutex
	now time.Time
}

func (c *clock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.now
}

func (c *clock) Advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.now = c.now.Add(d)
}

type fixture struct {
	t     *testing.T
	clock *clock
	h     http.Handler
}

// Test auth: "Authorization: Bearer chain:<id>" is chain <id>; anything
// else is an invalid token. The client IP is X-Test-IP.
func newFixture(t *testing.T, edit func(*pairing.Config)) *fixture {
	c := &clock{now: time.Unix(1_800_000_000, 0)}
	cfg := pairing.DefaultConfig()
	cfg.Now = c.Now
	cfg.PollTimeout = 2 * time.Second
	cfg.ClientIP = func(r *http.Request) string { return r.Header.Get("X-Test-IP") }
	cfg.Authorize = func(r *http.Request) (string, error) {
		v := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
		if id, ok := strings.CutPrefix(v, "chain:"); ok && id != "" {
			return id, nil
		}
		return "", errors.New("invalid token")
	}
	if edit != nil {
		edit(&cfg)
	}
	return &fixture{t: t, clock: c, h: pairing.New(cfg).Routes()}
}

type req struct {
	method, path, chain, bToken, ip string
	body                            any
}

func (f *fixture) do(r req) (int, map[string]any) {
	f.t.Helper()
	var body *strings.Reader
	switch b := r.body.(type) {
	case nil:
		body = strings.NewReader("")
	case string:
		body = strings.NewReader(b)
	default:
		raw, err := json.Marshal(b)
		require.NoError(f.t, err)
		body = strings.NewReader(string(raw))
	}
	hr := httptest.NewRequest(r.method, r.path, body)
	if r.chain != "" {
		hr.Header.Set("Authorization", "Bearer chain:"+r.chain)
	}
	if r.bToken != "" {
		hr.Header.Set(pairing.PairTokenHeader, r.bToken)
	}
	ip := r.ip
	if ip == "" {
		ip = "192.0.2.1"
	}
	hr.Header.Set("X-Test-IP", ip)
	rec := httptest.NewRecorder()
	f.h.ServeHTTP(rec, hr)
	var out map[string]any
	if rec.Body.Len() > 0 {
		require.NoError(f.t, json.Unmarshal(rec.Body.Bytes(), &out), rec.Body.String())
	}
	return rec.Code, out
}

func (f *fixture) open(chain string) int {
	f.t.Helper()
	code, out := f.do(req{method: "POST", path: "/", chain: chain})
	require.Equal(f.t, http.StatusCreated, code)
	assert.EqualValues(f.t, 300, out["expires_in"])
	return int(out["slot"].(float64))
}

func (f *fixture) join(slot string) string {
	f.t.Helper()
	code, out := f.do(req{method: "POST", path: "/" + slot + "/join"})
	require.Equal(f.t, http.StatusOK, code, out)
	return out["b_token"].(string)
}

func msg(from string, data []byte) map[string]string {
	return map[string]string{"from": from, "data": base64.StdEncoding.EncodeToString(data)}
}

func TestOpenNeedsAuth(t *testing.T) {
	f := newFixture(t, nil)
	code, _ := f.do(req{method: "POST", path: "/"})
	assert.Equal(t, http.StatusUnauthorized, code)
	code, _ = f.do(req{method: "POST", path: "/", chain: ""})
	assert.Equal(t, http.StatusUnauthorized, code)
}

func TestLowestFreeSlot(t *testing.T) {
	f := newFixture(t, nil)
	assert.Equal(t, 1, f.open("c1"))
	assert.Equal(t, 2, f.open("c2"))
	assert.Equal(t, 3, f.open("c3"))

	code, _ := f.do(req{method: "DELETE", path: "/2", chain: "c2"})
	require.Equal(t, http.StatusNoContent, code)
	assert.Equal(t, 2, f.open("c4"), "a closed slot's number is reused")

	// Expired slots free their numbers too.
	f.clock.Advance(301 * time.Second)
	assert.Equal(t, 1, f.open("c5"))
	assert.Equal(t, 2, f.open("c6"))
}

func TestOneSlotPerChain(t *testing.T) {
	f := newFixture(t, nil)
	assert.Equal(t, 1, f.open("c1"))
	assert.Equal(t, 2, f.open("c2"))
	f.join("2")

	// c2 opens again: its slot 2 closes, and 2 is the lowest free number.
	assert.Equal(t, 2, f.open("c2"))
	f.join("2") // A new slot: its first join succeeds.

	// Chain IDs compare case-insensitively (Chromium sends upper-case hex).
	assert.Equal(t, 3, f.open("ab"))
	assert.Equal(t, 3, f.open("AB"))
	code, _ := f.do(req{method: "POST", path: "/3/join"})
	assert.Equal(t, http.StatusOK, code)
	code, _ = f.do(req{method: "POST", path: "/1/msg", chain: "c1", body: msg("a", []byte("x"))})
	assert.Equal(t, http.StatusOK, code, "other chains' slots stay open")
}

func TestFirstJoinOnly(t *testing.T) {
	f := newFixture(t, nil)
	f.open("c1")
	token := f.join("1")
	assert.Regexp(t, "^[0-9a-f]{32}$", token)
	code, _ := f.do(req{method: "POST", path: "/1/join"})
	assert.Equal(t, http.StatusConflict, code)
}

func TestNotFoundAfterExpiryAndClose(t *testing.T) {
	f := newFixture(t, nil)
	everywhere := func(slot, token string) {
		t.Helper()
		for _, r := range []req{
			{method: "POST", path: "/" + slot + "/join"},
			{method: "POST", path: "/" + slot + "/msg", chain: "c1", body: msg("a", []byte("x"))},
			{method: "POST", path: "/" + slot + "/msg", bToken: token, body: msg("b", []byte("x"))},
			{method: "GET", path: "/" + slot + "/msg?for=a", chain: "c1"},
			{method: "GET", path: "/" + slot + "/msg?for=b", bToken: token},
			{method: "DELETE", path: "/" + slot, chain: "c1"},
		} {
			code, _ := f.do(r)
			assert.Equal(t, http.StatusNotFound, code, "%s %s", r.method, r.path)
		}
	}
	everywhere("1", "00")
	everywhere("0", "00")
	everywhere("x", "00")

	f.open("c1")
	token := f.join("1")
	f.clock.Advance(299 * time.Second)
	code, _ := f.do(req{method: "POST", path: "/1/msg", chain: "c1", body: msg("a", []byte("x"))})
	require.Equal(t, http.StatusOK, code, "still open at 299 s")
	f.clock.Advance(time.Second)
	everywhere("1", token)

	f.open("c1")
	token = f.join("1")
	code, _ = f.do(req{method: "DELETE", path: "/1", bToken: token})
	require.Equal(t, http.StatusNoContent, code, "B may close")
	everywhere("1", token)
}

func TestWhoMayPostAndRead(t *testing.T) {
	f := newFixture(t, func(c *pairing.Config) { c.PollTimeout = 10 * time.Millisecond })
	f.open("c1")
	f.open("c2")

	// Before the join there is no b_token to present.
	code, _ := f.do(req{method: "POST", path: "/1/msg", bToken: "00", body: msg("b", []byte("x"))})
	assert.Equal(t, http.StatusForbidden, code)

	token := f.join("1")
	other := f.join("2")
	for _, c := range []struct {
		r    req
		want int
	}{
		{req{method: "POST", path: "/1/msg", body: msg("b", []byte("x"))}, http.StatusUnauthorized},
		{req{method: "POST", path: "/1/msg", bToken: "0123", body: msg("b", []byte("x"))}, http.StatusForbidden},
		{req{method: "POST", path: "/1/msg", bToken: other, body: msg("b", []byte("x"))}, http.StatusForbidden},
		{req{method: "POST", path: "/1/msg", chain: "c1", body: msg("b", []byte("x"))}, http.StatusUnauthorized},
		{req{method: "POST", path: "/1/msg", bToken: token, body: msg("a", []byte("x"))}, http.StatusUnauthorized},
		{req{method: "POST", path: "/1/msg", chain: "c2", body: msg("a", []byte("x"))}, http.StatusForbidden},
		{req{method: "GET", path: "/1/msg?for=b"}, http.StatusUnauthorized},
		{req{method: "GET", path: "/1/msg?for=b", bToken: other}, http.StatusForbidden},
		{req{method: "GET", path: "/1/msg?for=a", bToken: token}, http.StatusUnauthorized},
		{req{method: "GET", path: "/1/msg?for=a", chain: "c2"}, http.StatusForbidden},
		{req{method: "DELETE", path: "/1"}, http.StatusUnauthorized},
		{req{method: "DELETE", path: "/1", chain: "c2"}, http.StatusForbidden},
		{req{method: "DELETE", path: "/1", bToken: other}, http.StatusForbidden},
		{req{method: "POST", path: "/1/msg", bToken: token, body: msg("b", []byte("x"))}, http.StatusOK},
		{req{method: "POST", path: "/1/msg", chain: "c1", body: msg("a", []byte("x"))}, http.StatusOK},
		{req{method: "GET", path: "/1/msg?for=b", bToken: token}, http.StatusOK},
		{req{method: "GET", path: "/1/msg?for=a", chain: "c1"}, http.StatusOK},
	} {
		code, out := f.do(c.r)
		assert.Equal(t, c.want, code, "%s %s from=%v: %v", c.r.method, c.r.path, c.r.body, out)
	}
}

func TestMessagesPerSide(t *testing.T) {
	f := newFixture(t, nil)
	f.open("c1")
	token := f.join("1")
	for i := 1; i <= 8; i++ {
		code, out := f.do(req{method: "POST", path: "/1/msg", chain: "c1", body: msg("a", []byte{byte(i)})})
		require.Equal(t, http.StatusOK, code)
		assert.EqualValues(t, i, out["seq"], "sequence numbers count per sender from 1")
	}
	code, _ := f.do(req{method: "POST", path: "/1/msg", chain: "c1", body: msg("a", []byte("9"))})
	assert.Equal(t, http.StatusTooManyRequests, code)

	code, out := f.do(req{method: "POST", path: "/1/msg", bToken: token, body: msg("b", []byte("1"))})
	require.Equal(t, http.StatusOK, code, "B has its own 8")
	assert.EqualValues(t, 1, out["seq"])
}

func TestMessageSize(t *testing.T) {
	f := newFixture(t, nil)
	f.open("c1")
	post := func(body any) int {
		code, _ := f.do(req{method: "POST", path: "/1/msg", chain: "c1", body: body})
		return code
	}
	assert.Equal(t, http.StatusOK, post(msg("a", make([]byte, 4096))))
	assert.Equal(t, http.StatusRequestEntityTooLarge, post(msg("a", make([]byte, 4097))))
	assert.Equal(t, http.StatusRequestEntityTooLarge,
		post(`{"from":"a","data":"`+strings.Repeat("A", 20000)+`"}`))
	assert.Equal(t, http.StatusBadRequest, post(map[string]string{"from": "a", "data": "not base64!"}))
	assert.Equal(t, http.StatusBadRequest, post(map[string]string{"from": "a", "data": ""}))
	assert.Equal(t, http.StatusBadRequest, post(msg("c", []byte("x"))))
	assert.Equal(t, http.StatusBadRequest, post("not json"))
}

func TestPollReturnsWaitingMessagesAndFiltersBySeq(t *testing.T) {
	f := newFixture(t, nil)
	f.open("c1")
	token := f.join("1")
	for _, d := range []string{"one", "two"} {
		code, _ := f.do(req{method: "POST", path: "/1/msg", chain: "c1", body: msg("a", []byte(d))})
		require.Equal(t, http.StatusOK, code)
	}
	code, out := f.do(req{method: "GET", path: "/1/msg?for=b", bToken: token})
	require.Equal(t, http.StatusOK, code)
	assert.Equal(t, []any{
		map[string]any{"seq": 1.0, "data": base64.StdEncoding.EncodeToString([]byte("one"))},
		map[string]any{"seq": 2.0, "data": base64.StdEncoding.EncodeToString([]byte("two"))},
	}, out["messages"])

	code, out = f.do(req{method: "GET", path: "/1/msg?for=b&after=1", bToken: token})
	require.Equal(t, http.StatusOK, code)
	assert.Len(t, out["messages"], 1)

	code, _ = f.do(req{method: "GET", path: "/1/msg?for=c", bToken: token})
	assert.Equal(t, http.StatusBadRequest, code)
	code, _ = f.do(req{method: "GET", path: "/1/msg?for=b&after=-1", bToken: token})
	assert.Equal(t, http.StatusBadRequest, code)
}

func TestLongPollReturnsOnNewMessage(t *testing.T) {
	f := newFixture(t, func(c *pairing.Config) { c.PollTimeout = 10 * time.Second })
	f.open("c1")
	token := f.join("1")

	type result struct {
		code int
		out  map[string]any
		took time.Duration
	}
	done := make(chan result)
	go func() {
		start := time.Now()
		code, out := f.do(req{method: "GET", path: "/1/msg?for=a", chain: "c1"})
		done <- result{code, out, time.Since(start)}
	}()
	time.Sleep(100 * time.Millisecond)
	code, _ := f.do(req{method: "POST", path: "/1/msg", bToken: token, body: msg("b", []byte("spake"))})
	require.Equal(t, http.StatusOK, code)

	select {
	case r := <-done:
		require.Equal(t, http.StatusOK, r.code)
		assert.Len(t, r.out["messages"], 1)
		assert.Less(t, r.took, 2*time.Second)
	case <-time.After(5 * time.Second):
		t.Fatal("long poll didn't return on a new message")
	}
}

func TestLongPollTimesOutEmpty(t *testing.T) {
	f := newFixture(t, func(c *pairing.Config) { c.PollTimeout = 150 * time.Millisecond })
	f.open("c1")
	start := time.Now()
	code, out := f.do(req{method: "GET", path: "/1/msg?for=a", chain: "c1"})
	assert.Equal(t, http.StatusOK, code)
	assert.Equal(t, []any{}, out["messages"])
	assert.GreaterOrEqual(t, time.Since(start), 150*time.Millisecond)
}

func TestDefaultPollTimeoutIs25s(t *testing.T) {
	assert.Equal(t, 25*time.Second, pairing.DefaultConfig().PollTimeout)
	assert.Equal(t, 300*time.Second, pairing.DefaultConfig().SlotTTL)
}

func TestLongPollEndsWhenSlotCloses(t *testing.T) {
	f := newFixture(t, func(c *pairing.Config) { c.PollTimeout = 10 * time.Second })
	f.open("c1")
	token := f.join("1")
	done := make(chan int)
	go func() {
		code, _ := f.do(req{method: "GET", path: "/1/msg?for=b", bToken: token})
		done <- code
	}()
	time.Sleep(100 * time.Millisecond)
	code, _ := f.do(req{method: "DELETE", path: "/1", chain: "c1"})
	require.Equal(t, http.StatusNoContent, code)
	select {
	case code := <-done:
		assert.Equal(t, http.StatusNotFound, code)
	case <-time.After(5 * time.Second):
		t.Fatal("long poll didn't end when the slot closed")
	}
}

func TestJoinRateLimitPerIP(t *testing.T) {
	f := newFixture(t, nil)
	f.open("c1")
	for range 10 {
		code, _ := f.do(req{method: "POST", path: "/99/join", ip: "198.51.100.1"})
		require.Equal(t, http.StatusNotFound, code)
	}
	// The 11th attempt is refused even for a real slot.
	code, _ := f.do(req{method: "POST", path: "/1/join", ip: "198.51.100.1"})
	assert.Equal(t, http.StatusTooManyRequests, code)
	// Other addresses aren't affected.
	code, _ = f.do(req{method: "POST", path: "/99/join", ip: "198.51.100.2"})
	assert.Equal(t, http.StatusNotFound, code)

	// After the window, the address may try again.
	f.clock.Advance(10*time.Minute + time.Second)
	f.open("c1")
	code, _ = f.do(req{method: "POST", path: "/1/join", ip: "198.51.100.1"})
	assert.Equal(t, http.StatusOK, code)
}

func TestOpenRateLimitPerIP(t *testing.T) {
	f := newFixture(t, func(c *pairing.Config) { c.OpensPerIP = 2 })
	f.open("c1")
	f.open("c2")
	code, _ := f.do(req{method: "POST", path: "/", chain: "c3"})
	assert.Equal(t, http.StatusTooManyRequests, code)
}

// A and B as the clients will run it (sync.md, Pairing steps 1-5), over a
// real HTTP server.
func TestRoundTrip(t *testing.T) {
	f := newFixture(t, nil)
	srv := httptest.NewServer(f.h)
	defer srv.Close()
	call := func(method, path string, hdr map[string]string, body any) (int, map[string]any) {
		var rd *strings.Reader
		if body != nil {
			raw, _ := json.Marshal(body)
			rd = strings.NewReader(string(raw))
		} else {
			rd = strings.NewReader("")
		}
		r, err := http.NewRequest(method, srv.URL+path, rd)
		require.NoError(t, err)
		for k, v := range hdr {
			r.Header.Set(k, v)
		}
		resp, err := http.DefaultClient.Do(r)
		require.NoError(t, err)
		defer resp.Body.Close()
		var out map[string]any
		_ = json.NewDecoder(resp.Body).Decode(&out)
		return resp.StatusCode, out
	}
	a := map[string]string{"Authorization": "Bearer chain:A1B2"}

	code, out := call("POST", "/", a, nil)
	require.Equal(t, http.StatusCreated, code)
	slot := "/" + strconv.Itoa(int(out["slot"].(float64)))
	code, out = call("POST", slot+"/join", nil, nil)
	require.Equal(t, http.StatusOK, code)
	b := map[string]string{pairing.PairTokenHeader: out["b_token"].(string)}

	var wg sync.WaitGroup
	var aGot, bGot []string
	wg.Add(2)
	go func() { // A: wait for B's message, answer, wait, answer.
		defer wg.Done()
		after := 0
		for len(aGot) < 2 {
			code, out := call("GET", slot+"/msg?for=a&after="+strconv.Itoa(after), a, nil)
			if code != http.StatusOK {
				t.Errorf("A poll: %d", code)
				return
			}
			for _, m := range out["messages"].([]any) {
				mm := m.(map[string]any)
				aGot = append(aGot, mm["data"].(string))
				after = int(mm["seq"].(float64))
				code, _ := call("POST", slot+"/msg", a, map[string]string{"from": "a", "data": mm["data"].(string)})
				if code != http.StatusOK {
					t.Errorf("A post: %d", code)
				}
			}
		}
	}()
	go func() { // B: send, wait for the echo, twice.
		defer wg.Done()
		for i, d := range []string{"bob", "confirm"} {
			data := base64.StdEncoding.EncodeToString([]byte(d))
			code, _ := call("POST", slot+"/msg", b, map[string]string{"from": "b", "data": data})
			if code != http.StatusOK {
				t.Errorf("B post: %d", code)
				return
			}
			code, out := call("GET", slot+"/msg?for=b&after="+strconv.Itoa(i), b, nil)
			if code != http.StatusOK || len(out["messages"].([]any)) != 1 {
				t.Errorf("B poll: %d %v", code, out)
				return
			}
			bGot = append(bGot, out["messages"].([]any)[0].(map[string]any)["data"].(string))
		}
	}()
	wg.Wait()
	want := []string{base64.StdEncoding.EncodeToString([]byte("bob")),
		base64.StdEncoding.EncodeToString([]byte("confirm"))}
	assert.Equal(t, want, aGot)
	assert.Equal(t, want, bGot)

	code, _ = call("DELETE", slot, a, nil)
	assert.Equal(t, http.StatusNoContent, code)
	code, _ = call("GET", slot+"/msg?for=b", b, nil)
	assert.Equal(t, http.StatusNotFound, code)
}
