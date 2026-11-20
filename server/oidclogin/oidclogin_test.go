package oidclogin_test

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"encoding/json"
	"io"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/auth"
	"github.com/brave/go-sync/auth/authtest"
	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/oidclogin"
	"github.com/brave/go-sync/oidclogin/fakeidp"
)

const (
	syncHost = "sync.example"
	issuer   = "http://idp.test/realms/acme"
	clientID = "compass"
)

type clock struct {
	mu sync.Mutex
	t  time.Time
}

func (c *clock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}

func (c *clock) Add(d time.Duration) {
	c.mu.Lock()
	c.t = c.t.Add(d)
	c.mu.Unlock()
}

// hosts routes requests by host to in-process handlers (the browser).
type hosts map[string]http.Handler

func (h hosts) RoundTrip(r *http.Request) (*http.Response, error) {
	handler, ok := h[r.URL.Host]
	if !ok {
		return nil, &url.Error{Op: "dial", URL: r.URL.String(), Err: io.EOF}
	}
	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, r)
	resp := rec.Result()
	resp.Request = r
	return resp, nil
}

type harness struct {
	t       *testing.T
	idp     *fakeidp.IDP
	db      *datastore.SQLite
	svc     *oidclogin.Service
	clock   *clock
	router  chi.Router
	browser *http.Client
}

func newHarness(t *testing.T, mutate func(*oidclogin.Config)) *harness {
	t.Helper()
	h := &harness{t: t, idp: fakeidp.New(issuer, clientID), clock: &clock{t: time.Now()}}
	db, err := datastore.NewSQLite(filepath.Join(t.TempDir(), "sync.db"), datastore.SQLiteOptions{})
	require.NoError(t, err)
	t.Cleanup(func() { db.Close() })
	h.db = db
	cfg := oidclogin.DefaultConfig()
	cfg.Issuer, cfg.ClientID, cfg.Name, cfg.PublicHost = issuer, clientID, "Acme SSO", syncHost
	cfg.Store = db
	cfg.HTTPClient = h.idp.Client()
	cfg.Now = h.clock.Now
	cfg.GateCacheTTL = 0
	cfg.PollTimeout = 2 * time.Second
	if mutate != nil {
		mutate(&cfg)
	}
	h.svc, err = oidclogin.New(context.Background(), cfg)
	require.NoError(t, err)
	h.router = chi.NewRouter()
	h.router.Route("/v2", func(r chi.Router) {
		h.svc.Register(r)
		// Stands for every sync endpoint (they all use Authorize).
		r.Get("/gated", func(w http.ResponseWriter, r *http.Request) {
			chain, err := h.svc.Authorize()(r)
			if err != nil {
				if !auth.WriteGateError(w, err) {
					http.Error(w, "invalid sync token", http.StatusUnauthorized)
				}
				return
			}
			_, _ = w.Write([]byte(chain))
		})
	})
	jar, err := cookiejar.New(nil)
	require.NoError(t, err)
	h.browser = &http.Client{
		Transport: hosts{syncHost: h.router, "idp.test": h.idp},
		Jar:       jar,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
	return h
}

type chain struct{ key ed25519.PrivateKey }

func newChain(t *testing.T) chain {
	_, key, err := ed25519.GenerateKey(rand.Reader)
	require.NoError(t, err)
	return chain{key}
}

func (c chain) token() string {
	tok, _ := authtest.TokenFor(c.key, time.Now().UnixMilli())
	return tok
}

func (c chain) id() string {
	_, id := authtest.TokenFor(c.key, time.Now().UnixMilli())
	return id
}

// api calls the router with |c|'s token (none if nil).
func (h *harness) api(method, path string, c *chain, body any, hdr ...string) *httptest.ResponseRecorder {
	h.t.Helper()
	var rd io.Reader
	if body != nil {
		b, err := json.Marshal(body)
		require.NoError(h.t, err)
		rd = bytes.NewReader(b)
	}
	req := httptest.NewRequest(method, path, rd)
	if c != nil {
		req.Header.Set("Authorization", "Bearer "+c.token())
	}
	for i := 0; i+1 < len(hdr); i += 2 {
		req.Header.Set(hdr[i], hdr[i+1])
	}
	rec := httptest.NewRecorder()
	h.router.ServeHTTP(rec, req)
	return rec
}

func decode(t *testing.T, rec *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var out map[string]any
	require.NoError(t, json.Unmarshal(rec.Body.Bytes(), &out), rec.Body.String())
	return out
}

type created struct {
	ID       string `json:"id"`
	URL      string `json:"url"`
	UserCode string `json:"user_code"`
	Expires  int64  `json:"expires"`
}

func (h *harness) create(c chain, ret string) created {
	h.t.Helper()
	rec := h.api("POST", "/v2/compass/login", &c, map[string]string{"device_name": "Work Mac", "return": ret})
	require.Equal(h.t, http.StatusOK, rec.Code, rec.Body.String())
	var out created
	require.NoError(h.t, json.Unmarshal(rec.Body.Bytes(), &out))
	return out
}

func (h *harness) pollNow(c chain, id string) (int, map[string]any) {
	rec := h.api("GET", "/v2/compass/login/"+id+"?wait=0", &c, nil)
	if rec.Code != http.StatusOK {
		return rec.Code, nil
	}
	return rec.Code, decode(h.t, rec)
}

func (h *harness) get(u string) *http.Response {
	h.t.Helper()
	resp, err := h.browser.Get(u)
	require.NoError(h.t, err)
	h.t.Cleanup(func() { resp.Body.Close() })
	return resp
}

func body(t *testing.T, resp *http.Response) string {
	t.Helper()
	b, err := io.ReadAll(resp.Body)
	require.NoError(t, err)
	return string(b)
}

// toIDP opens the login URL; returns the authorize request's parameters.
func (h *harness) toIDP(c created) url.Values {
	h.t.Helper()
	resp := h.get(c.URL)
	require.Equal(h.t, http.StatusFound, resp.StatusCode)
	loc, err := url.Parse(resp.Header.Get("Location"))
	require.NoError(h.t, err)
	require.Equal(h.t, "idp.test", loc.Host)
	return loc.Query()
}

// consent signs |who| in at the identity provider and follows the
// callback: the consent page's response.
func (h *harness) consent(c created, who fakeidp.Identity) *http.Response {
	h.t.Helper()
	back, err := h.idp.Approve(h.toIDP(c), who)
	require.NoError(h.t, err)
	return h.get(back)
}

var csrfRE = regexp.MustCompile(`name="csrf" value="([^"]+)"`)

func (h *harness) decide(c created, page, action string) *http.Response {
	h.t.Helper()
	m := csrfRE.FindStringSubmatch(page)
	require.NotNil(h.t, m, page)
	resp, err := h.browser.PostForm("https://"+syncHost+"/v2/compass/login/"+c.ID+"/"+action,
		url.Values{"csrf": {m[1]}})
	require.NoError(h.t, err)
	h.t.Cleanup(func() { resp.Body.Close() })
	return resp
}

var alice = fakeidp.Identity{Email: "alice@acme.com", Name: "Alice", EmailVerified: true, Groups: []string{"staff"}}

// signIn runs the whole flow for |c| as |who| and allows it.
func (h *harness) signIn(c chain, who fakeidp.Identity) {
	h.t.Helper()
	cr := h.create(c, "tab")
	resp := h.consent(cr, who)
	require.Equal(h.t, http.StatusOK, resp.StatusCode)
	done := h.decide(cr, body(h.t, resp), "approve")
	require.Equal(h.t, http.StatusOK, done.StatusCode)
}

func TestInfo(t *testing.T) {
	h := newHarness(t, nil)
	rec := h.api("GET", "/v2/compass/info", nil, nil)
	assert.Equal(t, http.StatusOK, rec.Code)
	assert.JSONEq(t, `{"login":{"type":"oidc","name":"Acme SSO"}}`, rec.Body.String())

	h = newHarness(t, func(c *oidclogin.Config) { c.Name = "" })
	assert.JSONEq(t, `{"login":{"type":"oidc","name":"idp.test"}}`, h.api("GET", "/v2/compass/info", nil, nil).Body.String())
}

func TestHappyPath(t *testing.T) {
	h := newHarness(t, nil)
	c := newChain(t)

	// Unbound: every sync endpoint says login_required.
	rec := h.api("GET", "/v2/gated", &c, nil)
	assert.Equal(t, http.StatusUnauthorized, rec.Code)
	assert.JSONEq(t, `{"error":"login_required"}`, rec.Body.String())
	assert.Equal(t, "required", rec.Header().Get("X-Compass-Login"))
	rec = h.api("GET", "/v2/compass/account", &c, nil)
	assert.Equal(t, http.StatusOK, rec.Code)
	assert.JSONEq(t, `{"login_required":true,"user":null,"expires":0}`, rec.Body.String())

	cr := h.create(c, "tab")
	assert.Equal(t, "https://sync.example/v2/compass/login/"+cr.ID+"/start", cr.URL)
	assert.Regexp(t, `^[A-HJKMNP-Z2-9]{3}-[A-HJKMNP-Z2-9]{3}$`, cr.UserCode)
	assert.InDelta(t, h.clock.Now().Add(10*time.Minute).UnixMilli(), cr.Expires, 1000)
	code, st := h.pollNow(c, cr.ID)
	assert.Equal(t, http.StatusOK, code)
	assert.Equal(t, map[string]any{"status": "pending", "user": nil}, st)

	q := h.toIDP(cr)
	assert.Equal(t, "S256", q.Get("code_challenge_method"))
	assert.NotEmpty(t, q.Get("nonce"))
	assert.NotEmpty(t, q.Get("state"))
	assert.Equal(t, "https://sync.example/v2/compass/login/callback", q.Get("redirect_uri"))
	assert.Contains(t, q.Get("scope"), "offline_access")
	back, err := h.idp.Approve(q, alice)
	require.NoError(t, err)
	resp := h.get(back)
	require.Equal(t, http.StatusOK, resp.StatusCode)
	assert.Contains(t, resp.Header.Get("Content-Security-Policy"), "default-src 'none'")
	assert.Contains(t, resp.Header.Get("Content-Security-Policy"), "frame-ancestors 'none'")
	page := body(t, resp)
	for _, want := range []string{"Acme SSO", "alice@acme.com", "Work Mac", cr.UserCode,
		"Check that Compass shows the same code"} {
		assert.Contains(t, page, want)
	}
	// Still pending until allowed.
	_, st = h.pollNow(c, cr.ID)
	assert.Equal(t, "pending", st["status"])

	// A long poll wakes when the person allows.
	polled := make(chan map[string]any, 1)
	go func() {
		req := httptest.NewRequest("GET", "/v2/compass/login/"+cr.ID, nil)
		req.Header.Set("Authorization", "Bearer "+c.token())
		rec := httptest.NewRecorder()
		h.router.ServeHTTP(rec, req)
		var out map[string]any
		_ = json.Unmarshal(rec.Body.Bytes(), &out)
		polled <- out
	}()
	time.Sleep(50 * time.Millisecond)
	done := h.decide(cr, page, "approve")
	assert.Equal(t, http.StatusOK, done.StatusCode)
	assert.Contains(t, body(t, done), "You can return to Compass")
	want := map[string]any{"status": "done", "user": map[string]any{"name": "Alice", "email": "alice@acme.com"}}
	select {
	case got := <-polled:
		assert.Equal(t, want, got)
	case <-time.After(5 * time.Second):
		t.Fatal("the long poll didn't wake")
	}
	_, st = h.pollNow(c, cr.ID)
	assert.Equal(t, want, st)

	rec = h.api("GET", "/v2/gated", &c, nil)
	assert.Equal(t, http.StatusOK, rec.Code)
	assert.Equal(t, strings.ToUpper(c.id()), rec.Body.String(), "Authorize's chain ID, as sent")
	rec = h.api("GET", "/v2/compass/account", &c, nil)
	assert.JSONEq(t, `{"login_required":false,"user":{"name":"Alice","email":"alice@acme.com"},"expires":0}`,
		rec.Body.String())

	// The consent page is single use.
	again := h.decide(cr, page, "approve")
	assert.Equal(t, http.StatusGone, again.StatusCode)
}

func TestReturnCompass(t *testing.T) {
	h := newHarness(t, nil)
	c := newChain(t)
	cr := h.create(c, "compass")
	resp := h.consent(cr, alice)
	require.Equal(t, http.StatusOK, resp.StatusCode)
	assert.Contains(t, resp.Header.Get("Content-Security-Policy"), "form-action 'self' compass:")
	done := h.decide(cr, body(t, resp), "approve")
	assert.Equal(t, http.StatusFound, done.StatusCode)
	assert.Equal(t, "compass://sync-login-done?status=done", done.Header.Get("Location"))

	cr = h.create(c, "compass")
	resp = h.consent(cr, alice)
	done = h.decide(cr, body(t, resp), "deny")
	assert.Equal(t, "compass://sync-login-done?status=denied", done.Header.Get("Location"))
}

func TestDeny(t *testing.T) {
	h := newHarness(t, nil)
	c := newChain(t)
	cr := h.create(c, "tab")
	resp := h.consent(cr, alice)
	done := h.decide(cr, body(t, resp), "deny")
	assert.Equal(t, http.StatusOK, done.StatusCode)
	assert.Contains(t, body(t, done), "cancelled")
	_, st := h.pollNow(c, cr.ID)
	assert.Equal(t, map[string]any{"status": "denied", "user": nil}, st)
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &c, nil).Code)
}

func TestBadCallbacks(t *testing.T) {
	h := newHarness(t, nil)
	c := newChain(t)

	t.Run("wrong nonce", func(t *testing.T) {
		cr := h.create(c, "tab")
		h.idp.SetNonce("someone-elses")
		defer h.idp.SetNonce("")
		resp := h.consent(cr, alice)
		assert.Equal(t, http.StatusBadRequest, resp.StatusCode)
		assert.Contains(t, body(t, resp), "Sign-in failed")
		_, st := h.pollNow(c, cr.ID)
		assert.Equal(t, "pending", st["status"])
	})
	t.Run("wrong state", func(t *testing.T) {
		cr := h.create(c, "tab")
		q := h.toIDP(cr)
		q.Set("state", "forged")
		back, err := h.idp.Approve(q, alice)
		require.NoError(t, err)
		assert.Equal(t, http.StatusBadRequest, h.get(back).StatusCode)
	})
	t.Run("state used twice", func(t *testing.T) {
		cr := h.create(c, "tab")
		back, err := h.idp.Approve(h.toIDP(cr), alice)
		require.NoError(t, err)
		assert.Equal(t, http.StatusOK, h.get(back).StatusCode)
		assert.Equal(t, http.StatusBadRequest, h.get(back).StatusCode)
	})
	t.Run("another browser", func(t *testing.T) {
		cr := h.create(c, "tab")
		back, err := h.idp.Approve(h.toIDP(cr), alice)
		require.NoError(t, err)
		other := *h.browser
		other.Jar, _ = cookiejar.New(nil)
		resp, err := other.Get(back)
		require.NoError(t, err)
		defer resp.Body.Close()
		assert.Equal(t, http.StatusBadRequest, resp.StatusCode)
	})
	t.Run("identity provider error", func(t *testing.T) {
		cr := h.create(c, "tab")
		q := h.toIDP(cr)
		resp := h.get("https://" + syncHost + "/v2/compass/login/callback?state=" + q.Get("state") +
			"&error=access_denied&error_description=" + url.QueryEscape("<b>no</b>"))
		assert.Equal(t, http.StatusForbidden, resp.StatusCode)
		page := body(t, resp)
		assert.Contains(t, page, "access_denied")
		assert.NotContains(t, page, "<b>no</b>")
	})
	t.Run("wrong csrf", func(t *testing.T) {
		cr := h.create(c, "tab")
		resp := h.consent(cr, alice)
		require.Equal(t, http.StatusOK, resp.StatusCode)
		forged := h.decide(cr, `name="csrf" value="forged"`, "approve")
		assert.Equal(t, http.StatusForbidden, forged.StatusCode)
		_, st := h.pollNow(c, cr.ID)
		assert.Equal(t, "pending", st["status"])
	})
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &c, nil).Code)
}

func TestPolicy(t *testing.T) {
	h := newHarness(t, func(c *oidclogin.Config) {
		c.AllowedDomains = []string{"acme.com"}
		c.AllowedGroups = []string{"staff", "admins"}
	})
	c := newChain(t)
	for name, who := range map[string]fakeidp.Identity{
		"other domain":       {Email: "eve@evil.com", EmailVerified: true, Groups: []string{"staff"}},
		"unverified email":   {Email: "eve@acme.com", EmailVerified: false, Groups: []string{"staff"}},
		"not in a group":     {Email: "bob@acme.com", EmailVerified: true, Groups: []string{"guests"}},
		"no groups at all":   {Email: "bob@acme.com", EmailVerified: true},
		"subdomain isn't ok": {Email: "eve@x.acme.com", EmailVerified: true, Groups: []string{"staff"}},
	} {
		t.Run(name, func(t *testing.T) {
			cr := h.create(c, "tab")
			resp := h.consent(cr, who)
			assert.Equal(t, http.StatusForbidden, resp.StatusCode)
			assert.Contains(t, body(t, resp), "Sign-in failed")
		})
	}
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &c, nil).Code)
	h.signIn(c, fakeidp.Identity{Email: "Bob@ACME.com", EmailVerified: true, Groups: []string{"admins"}})
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &c, nil).Code)
}

func TestExpiredLogin(t *testing.T) {
	h := newHarness(t, nil)
	c := newChain(t)
	cr := h.create(c, "tab")
	q := h.toIDP(cr)
	h.clock.Add(11 * time.Minute)
	_, st := h.pollNow(c, cr.ID)
	assert.Equal(t, map[string]any{"status": "expired", "user": nil}, st)
	// A long poll answers at once too.
	rec := h.api("GET", "/v2/compass/login/"+cr.ID, &c, nil)
	assert.JSONEq(t, `{"status":"expired","user":null}`, rec.Body.String())
	assert.Equal(t, http.StatusGone, h.get(cr.URL).StatusCode)
	back, err := h.idp.Approve(q, alice)
	require.NoError(t, err)
	assert.Equal(t, http.StatusBadRequest, h.get(back).StatusCode)
	// Long after, it's forgotten.
	h.clock.Add(time.Hour)
	h.create(c, "tab") // Sweeps.
	code, _ := h.pollNow(c, cr.ID)
	assert.Equal(t, http.StatusNotFound, code)
}

func TestPollByAnotherChain(t *testing.T) {
	h := newHarness(t, nil)
	a, b := newChain(t), newChain(t)
	cr := h.create(a, "tab")
	code, _ := h.pollNow(b, cr.ID)
	assert.Equal(t, http.StatusNotFound, code)
	code, _ = h.pollNow(a, "nope")
	assert.Equal(t, http.StatusNotFound, code)
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/compass/login/"+cr.ID, nil, nil).Code)
}

func TestCreateRequests(t *testing.T) {
	h := newHarness(t, func(c *oidclogin.Config) { c.LoginsPerChain = 2 })
	c := newChain(t)
	assert.Equal(t, http.StatusUnauthorized, h.api("POST", "/v2/compass/login", nil, map[string]string{}).Code)
	assert.Equal(t, http.StatusBadRequest,
		h.api("POST", "/v2/compass/login", &c, map[string]string{"return": "elsewhere"}).Code)
	h.create(c, "tab")
	h.create(c, "")
	rec := h.api("POST", "/v2/compass/login", &c, map[string]string{})
	assert.Equal(t, http.StatusTooManyRequests, rec.Code)
	h.clock.Add(11 * time.Minute)
	h.create(c, "tab")
}

func TestLinkAndSignOut(t *testing.T) {
	h := newHarness(t, nil)
	main, profile, stranger := newChain(t), newChain(t), newChain(t)

	// An unbound chain can't link.
	rec := h.api("POST", "/v2/compass/account/link", &stranger, nil, oidclogin.LinkedTokenHeader, profile.token())
	assert.Equal(t, http.StatusUnauthorized, rec.Code)
	assert.JSONEq(t, `{"error":"login_required"}`, rec.Body.String())

	h.signIn(main, alice)
	rec = h.api("POST", "/v2/compass/account/link", &main, nil, oidclogin.LinkedTokenHeader, "garbage")
	assert.Equal(t, http.StatusBadRequest, rec.Code)
	rec = h.api("POST", "/v2/compass/account/link", &main, nil, oidclogin.LinkedTokenHeader, profile.token())
	assert.Equal(t, http.StatusNoContent, rec.Code)
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &profile, nil).Code)
	assert.JSONEq(t, `{"login_required":false,"user":{"name":"Alice","email":"alice@acme.com"},"expires":0}`,
		h.api("GET", "/v2/compass/account", &profile, nil).Body.String())

	// Signing out one chain leaves the other.
	assert.Equal(t, http.StatusNoContent, h.api("DELETE", "/v2/compass/account", &main, nil).Code)
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &main, nil).Code)
	assert.JSONEq(t, `{"login_required":true,"user":null,"expires":0}`,
		h.api("GET", "/v2/compass/account", &main, nil).Body.String())
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &profile, nil).Code)
	assert.Equal(t, http.StatusUnauthorized, h.api("DELETE", "/v2/compass/account", nil, nil).Code)
}

// A private server without sign-in (COMPASS_SYNC_ALLOWED_CHAINS): a listed
// chain links its profile accounts, which then pass the allowlist (sync.md,
// "Profiles"); an unlisted one can't link.
func TestLinkOnAPrivateServer(t *testing.T) {
	h := newHarness(t, func(c *oidclogin.Config) { c.Issuer = "" })
	main, profile, stranger := newChain(t), newChain(t), newChain(t)
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", main.id())
	t.Setenv("BLOCKED_CLIENT_IDS", "")
	auth.SetLinkedRoot(h.db.ChainLinkRoot)
	t.Cleanup(func() { auth.SetLinkedRoot(nil) })

	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &main, nil).Code)
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &profile, nil).Code)
	// Not listed: can't link.
	rec := h.api("POST", "/v2/compass/account/link", &stranger, nil, oidclogin.LinkedTokenHeader, profile.token())
	assert.Equal(t, http.StatusUnauthorized, rec.Code)
	rec = h.api("POST", "/v2/compass/account/link", &main, nil, oidclogin.LinkedTokenHeader, "garbage")
	assert.Equal(t, http.StatusBadRequest, rec.Code)
	rec = h.api("POST", "/v2/compass/account/link", &main, nil, oidclogin.LinkedTokenHeader, profile.token())
	assert.Equal(t, http.StatusNoContent, rec.Code)
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &profile, nil).Code)
	// Again: the same.
	rec = h.api("POST", "/v2/compass/account/link", &main, nil, oidclogin.LinkedTokenHeader, profile.token())
	assert.Equal(t, http.StatusNoContent, rec.Code)
	// The linked chain may link another (its root stays main).
	third := newChain(t)
	rec = h.api("POST", "/v2/compass/account/link", &profile, nil, oidclogin.LinkedTokenHeader, third.token())
	assert.Equal(t, http.StatusNoContent, rec.Code)
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &third, nil).Code)

	// Blocking the root, or taking it off the list, shuts its links out.
	t.Setenv("BLOCKED_CLIENT_IDS", main.id())
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &profile, nil).Code)
	t.Setenv("BLOCKED_CLIENT_IDS", "")
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", stranger.id())
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &third, nil).Code)
	// A blocked chain can't be linked.
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", main.id())
	blocked := newChain(t)
	t.Setenv("BLOCKED_CLIENT_IDS", blocked.id())
	rec = h.api("POST", "/v2/compass/account/link", &main, nil, oidclogin.LinkedTokenHeader, blocked.token())
	assert.Equal(t, http.StatusBadRequest, rec.Code)
}

func TestRevokedUser(t *testing.T) {
	h := newHarness(t, func(c *oidclogin.Config) { c.GateCacheTTL = time.Minute })
	c := newChain(t)
	h.signIn(c, alice)
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &c, nil).Code)

	// As "users revoke" does, from another process.
	users, err := h.db.FindOIDCUsers(context.Background(), "ALICE@acme.com")
	require.NoError(t, err)
	require.Len(t, users, 1)
	chains, err := h.db.EndOIDCUserSessions(context.Background(), users[0].ID, true)
	require.NoError(t, err)
	assert.Equal(t, []string{c.id()}, chains)
	// The gate's cache lags by its TTL.
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &c, nil).Code)
	h.clock.Add(time.Minute)
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &c, nil).Code)

	// Signing in again is refused until restored.
	cr := h.create(c, "tab")
	resp := h.consent(cr, alice)
	assert.Equal(t, http.StatusForbidden, resp.StatusCode)
	assert.Contains(t, body(t, resp), "revoked")
	require.NoError(t, h.db.RestoreOIDCUser(context.Background(), users[0].ID))
	h.signIn(c, alice)
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &c, nil).Code)
}

func TestNoRefreshToken(t *testing.T) {
	h := newHarness(t, nil)
	h.idp.SetNoRefreshTokens(true)
	c := newChain(t)
	h.signIn(c, alice)
	acct := decode(t, h.api("GET", "/v2/compass/account", &c, nil))
	assert.InDelta(t, float64(h.clock.Now().AddDate(0, 0, 30).UnixMilli()), acct["expires"], 1000)
	h.clock.Add(29 * 24 * time.Hour)
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &c, nil).Code)
	h.clock.Add(2 * 24 * time.Hour)
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &c, nil).Code)
	assert.Equal(t, true, decode(t, h.api("GET", "/v2/compass/account", &c, nil))["login_required"])
}

func TestRevalidate(t *testing.T) {
	h := newHarness(t, func(c *oidclogin.Config) { c.AllowedGroups = []string{"staff"} })
	ctx := context.Background()
	a, b, c := newChain(t), newChain(t), newChain(t)
	h.signIn(a, alice)
	bob := fakeidp.Identity{Email: "bob@acme.com", Name: "Bob", EmailVerified: true, Groups: []string{"staff"}}
	h.signIn(b, bob)
	carol := fakeidp.Identity{Email: "carol@acme.com", Name: "Carol", EmailVerified: true, Groups: []string{"staff"}}
	h.signIn(c, carol)

	// Not due yet.
	assert.Equal(t, oidclogin.RevalidateResult{}, h.svc.Revalidate(ctx))

	// The identity provider is down: everyone keeps access.
	h.clock.Add(25 * time.Hour)
	h.idp.SetDown(true)
	assert.Equal(t, oidclogin.RevalidateResult{Failed: 3}, h.svc.Revalidate(ctx))
	for _, ch := range []chain{a, b, c} {
		assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &ch, nil).Code)
	}

	// Bob was removed (invalid_grant), Carol left the group, Alice is fine.
	h.idp.SetDown(false)
	h.idp.RevokeUser("bob@acme.com")
	h.idp.SetGroups("carol@acme.com", []string{"alumni"})
	assert.Equal(t, oidclogin.RevalidateResult{Checked: 1, Ended: 2}, h.svc.Revalidate(ctx))
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &a, nil).Code)
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &b, nil).Code)
	assert.Equal(t, http.StatusUnauthorized, h.api("GET", "/v2/gated", &c, nil).Code)

	// Alice's rotated refresh token works next time.
	assert.Equal(t, oidclogin.RevalidateResult{}, h.svc.Revalidate(ctx))
	h.clock.Add(25 * time.Hour)
	assert.Equal(t, oidclogin.RevalidateResult{Checked: 1}, h.svc.Revalidate(ctx))
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &a, nil).Code)

	// Bob can sign in again (not revoked by an admin).
	h.signIn(b, bob)
	assert.Equal(t, http.StatusOK, h.api("GET", "/v2/gated", &b, nil).Code)
}

func TestIssuerChange(t *testing.T) {
	h := newHarness(t, nil)
	c := newChain(t)
	h.signIn(c, alice)
	other, err := oidclogin.New(context.Background(), func() oidclogin.Config {
		cfg := oidclogin.DefaultConfig()
		cfg.Issuer, cfg.ClientID, cfg.PublicHost = "http://idp.test/realms/other", clientID, syncHost
		cfg.Store, cfg.HTTPClient = h.db, fakeidp.New("http://idp.test/realms/other", clientID).Client()
		return cfg
	}())
	require.NoError(t, err)
	assert.ErrorIs(t, other.Gate(context.Background(), c.id()), auth.ErrLoginRequired)
}

func TestDisabled(t *testing.T) {
	svc, err := oidclogin.New(context.Background(), oidclogin.DefaultConfig())
	require.NoError(t, err)
	assert.False(t, svc.Enabled())
	r := chi.NewRouter()
	r.Route("/v2", svc.Register)
	c := newChain(t)
	call := func(method, path string) *httptest.ResponseRecorder {
		req := httptest.NewRequest(method, path, strings.NewReader("{}"))
		req.Header.Set("Authorization", "Bearer "+c.token())
		rec := httptest.NewRecorder()
		r.ServeHTTP(rec, req)
		return rec
	}
	assert.JSONEq(t, `{"login":null}`, call("GET", "/v2/compass/info").Body.String())
	assert.JSONEq(t, `{"login_required":false,"user":null,"expires":0}`, call("GET", "/v2/compass/account").Body.String())
	rec := call("POST", "/v2/compass/login")
	assert.Equal(t, http.StatusNotFound, rec.Code)
	assert.JSONEq(t, `{"error":"login_disabled"}`, rec.Body.String())
	assert.Equal(t, http.StatusNoContent, call("DELETE", "/v2/compass/account").Code)

	// The sync endpoints' check is plain auth.Authorize.
	req := httptest.NewRequest("GET", "/", nil)
	req.Header.Set("Authorization", "Bearer "+c.token())
	id, err := svc.Authorize()(req)
	require.NoError(t, err)
	assert.Equal(t, strings.ToUpper(c.id()), id)
}

func TestConfigFromEnv(t *testing.T) {
	for _, k := range []string{"ISSUER", "CLIENT_ID", "CLIENT_SECRET", "CLIENT_SECRET_FILE", "NAME", "SCOPES",
		"ALLOWED_DOMAINS", "ALLOWED_GROUPS", "GROUPS_CLAIM", "SESSION_DAYS", "REVALIDATE_HOURS"} {
		t.Setenv("COMPASS_SYNC_OIDC_"+k, "")
	}
	t.Setenv("COMPASS_SYNC_DEV_FAKE_OIDC", "")
	cfg, err := oidclogin.ConfigFromEnv("")
	require.NoError(t, err)
	assert.False(t, cfg.Enabled())

	t.Setenv("COMPASS_SYNC_OIDC_ISSUER", "https://login.acme.com")
	_, err = oidclogin.ConfigFromEnv("sync.acme.com")
	assert.ErrorContains(t, err, "CLIENT_ID")
	t.Setenv("COMPASS_SYNC_OIDC_CLIENT_ID", "compass")
	_, err = oidclogin.ConfigFromEnv("")
	assert.ErrorContains(t, err, "PUBLIC_HOST")

	t.Setenv("COMPASS_SYNC_OIDC_ALLOWED_DOMAINS", " Acme.com, @corp.acme.com ,")
	t.Setenv("COMPASS_SYNC_OIDC_ALLOWED_GROUPS", "staff,Admins")
	t.Setenv("COMPASS_SYNC_OIDC_SESSION_DAYS", "7")
	t.Setenv("COMPASS_SYNC_OIDC_REVALIDATE_HOURS", "6")
	secret := filepath.Join(t.TempDir(), "secret")
	require.NoError(t, os.WriteFile(secret, []byte("s3cret\n"), 0o600))
	t.Setenv("COMPASS_SYNC_OIDC_CLIENT_SECRET_FILE", secret)
	cfg, err = oidclogin.ConfigFromEnv("sync.acme.com")
	require.NoError(t, err)
	assert.Equal(t, []string{"acme.com", "corp.acme.com"}, cfg.AllowedDomains)
	assert.Equal(t, []string{"staff", "Admins"}, cfg.AllowedGroups)
	assert.Equal(t, []string{"openid", "email", "profile", "offline_access"}, cfg.Scopes)
	assert.Equal(t, "groups", cfg.GroupsClaim)
	assert.Equal(t, 7, cfg.SessionDays)
	assert.Equal(t, 6*time.Hour, cfg.RevalidateEvery)
	assert.Equal(t, "s3cret", cfg.ClientSecret)

	// The fake identity provider: localhost only, instead of an issuer.
	t.Setenv("COMPASS_SYNC_DEV_FAKE_OIDC", "1")
	_, err = oidclogin.ConfigFromEnv("localhost:8295")
	assert.ErrorContains(t, err, "replaces")
	t.Setenv("COMPASS_SYNC_OIDC_ISSUER", "")
	_, err = oidclogin.ConfigFromEnv("sync.acme.com")
	assert.ErrorContains(t, err, "localhost")
	cfg, err = oidclogin.ConfigFromEnv("localhost:8295")
	require.NoError(t, err)
	assert.Equal(t, "http://localhost:8295/dev/fake-idp", cfg.Issuer)
	assert.Equal(t, "http://localhost:8295", oidclogin.BaseURL("localhost:8295"))
	assert.Equal(t, "https://sync.acme.com", oidclogin.BaseURL("sync.acme.com"))
}
