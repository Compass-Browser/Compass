package server_test

// Company sign-in end to end, on a server with the development identity
// provider (COMPASS_SYNC_DEV_FAKE_OIDC): an unbound chain gets
// login_required everywhere; a browser signs in through the fake provider's
// form and allows the device; the chain then syncs; the admin commands
// revoke it (sync.md, "Company sign-in (OIDC)").

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"net/url"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/oidclogin"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
	"github.com/brave/go-sync/server"
)

// oidcServer runs a server with the fake identity provider; returns its URL
// and database path.
func oidcServer(t *testing.T) (string, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "oidc.db")
	db, err := datastore.NewSQLite(path, datastore.SQLiteOptions{})
	require.NoError(t, err)
	t.Cleanup(func() { db.Close() })
	cfg, err := server.LoadConfig()
	require.NoError(t, err)
	// As COMPASS_SYNC_DEV_FAKE_OIDC=1 with COMPASS_SYNC_PUBLIC_HOST=localhost.
	cfg.Pairing.PublicHost = "localhost"
	cfg.OIDC.PublicHost = "localhost"
	cfg.OIDC.DevFakeIDP = true
	cfg.OIDC.Issuer = "http://localhost" + oidclogin.FakeIDPPath
	cfg.OIDC.ClientID = oidclogin.FakeIDPClientID
	cfg.OIDC.Name = "Fake IdP"
	cfg.OIDC.GateCacheTTL = 0
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	ctx, logger := server.SetupLogger(ctx)
	_, r, hub, err := server.SetupRouter(ctx, logger, cfg, db)
	require.NoError(t, err)
	srv := httptest.NewServer(r)
	t.Cleanup(func() { hub.Close(); srv.Close() })
	return srv.URL, path
}

// toServer sends requests for http://localhost (the public host) to the
// test server: the browser.
type toServer struct{ host string }

func (ts toServer) RoundTrip(r *http.Request) (*http.Response, error) {
	if r.URL.Host == "localhost" {
		r = r.Clone(r.Context())
		r.URL.Host = ts.host
	}
	return http.DefaultTransport.RoundTrip(r)
}

func browser(t *testing.T, base string) *http.Client {
	jar, err := cookiejar.New(nil)
	require.NoError(t, err)
	u, err := url.Parse(base)
	require.NoError(t, err)
	return &http.Client{
		Transport: toServer{u.Host},
		Jar:       jar,
		CheckRedirect: func(*http.Request, []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
}

func (d *device) call(method, path string, body []byte, hdr ...string) (*http.Response, string) {
	d.t.Helper()
	req, err := http.NewRequest(method, d.url+path, bytes.NewReader(body))
	require.NoError(d.t, err)
	req.Header.Set("Authorization", "Bearer "+d.token(time.Now()))
	for i := 0; i+1 < len(hdr); i += 2 {
		req.Header.Set(hdr[i], hdr[i+1])
	}
	resp, err := http.DefaultClient.Do(req)
	require.NoError(d.t, err)
	defer resp.Body.Close()
	b, err := io.ReadAll(resp.Body)
	require.NoError(d.t, err)
	return resp, string(b)
}

func readAll(t *testing.T, resp *http.Response) string {
	t.Helper()
	defer resp.Body.Close()
	b, err := io.ReadAll(resp.Body)
	require.NoError(t, err)
	return string(b)
}

// signInWithBrowser runs the browser part with the fake provider's form.
func signInWithBrowser(t *testing.T, base, loginURL, email string) {
	t.Helper()
	b := browser(t, base)
	resp, err := b.Get(loginURL)
	require.NoError(t, err)
	resp.Body.Close()
	require.Equal(t, http.StatusFound, resp.StatusCode)
	authorize := resp.Header.Get("Location")
	require.True(t, strings.HasPrefix(authorize, "http://localhost/dev/fake-idp/authorize?"), authorize)

	resp, err = b.Get(authorize)
	require.NoError(t, err)
	page := readAll(t, resp)
	require.Equal(t, http.StatusOK, resp.StatusCode)
	form := url.Values{"email": {email}, "name": {"Alice"}, "email_verified": {"true"}}
	for _, m := range regexp.MustCompile(`type="hidden" name="([^"]+)" value="([^"]*)"`).FindAllStringSubmatch(page, -1) {
		v, err := url.QueryUnescape(strings.ReplaceAll(m[2], "&amp;", "&"))
		require.NoError(t, err)
		form.Set(m[1], v)
	}
	resp, err = b.PostForm(authorize, form)
	require.NoError(t, err)
	resp.Body.Close()
	require.Equal(t, http.StatusFound, resp.StatusCode)
	callback := resp.Header.Get("Location")
	require.True(t, strings.HasPrefix(callback, "http://localhost/v2/compass/login/callback?"), callback)

	resp, err = b.Get(callback)
	require.NoError(t, err)
	page = readAll(t, resp)
	require.Equal(t, http.StatusOK, resp.StatusCode, page)
	m := regexp.MustCompile(`action="([^"]+/approve)"><input type="hidden" name="csrf" value="([^"]+)"`).FindStringSubmatch(page)
	require.NotNil(t, m, page)
	resp, err = b.PostForm("http://localhost"+m[1], url.Values{"csrf": {m[2]}})
	require.NoError(t, err)
	assert.Contains(t, readAll(t, resp), "You can return to Compass")
	require.Equal(t, http.StatusOK, resp.StatusCode)
}

func getUpdatesMsg() *sync_pb.ClientToServerMessage {
	contents := sync_pb.ClientToServerMessage_GET_UPDATES
	origin := sync_pb.SyncEnums_PERIODIC
	return &sync_pb.ClientToServerMessage{MessageContents: &contents,
		GetUpdates: &sync_pb.GetUpdatesMessage{GetUpdatesOrigin: &origin}}
}

func TestEndToEndOIDC(t *testing.T) {
	base, dbPath := oidcServer(t)
	seed := randomSeed(t)
	a := newDevice(t, base, seed, "cache-guid-a")

	resp, body := a.call("GET", "/v2/compass/info", nil)
	assert.Equal(t, http.StatusOK, resp.StatusCode)
	assert.JSONEq(t, `{"login":{"type":"oidc","name":"Fake IdP"}}`, body)

	// Unbound: login_required on every sync endpoint, never DISABLED_BY_ADMIN.
	loginRequired := `{"error":"login_required"}`
	for _, ep := range []struct{ method, path string }{
		{"POST", "/v2/command/"},
		{"GET", "/v2/compass/events"},
		{"GET", "/v2/compass/quota"},
		{"POST", "/v2/compass/push"},
		{"POST", "/v2/compass/pair"},
	} {
		resp, body := a.call(ep.method, ep.path, nil)
		assert.Equal(t, http.StatusUnauthorized, resp.StatusCode, ep.path)
		assert.JSONEq(t, loginRequired, body, ep.path)
		assert.Equal(t, "required", resp.Header.Get("X-Compass-Login"), ep.path)
	}
	code, _ := a.post(getUpdatesMsg(), a.token(time.Now()))
	assert.Equal(t, http.StatusUnauthorized, code)
	_, body = a.call("GET", "/v2/compass/account", nil)
	assert.JSONEq(t, `{"login_required":true,"user":null,"expires":0}`, body)

	// Sign in.
	resp, body = a.call("POST", "/v2/compass/login", []byte(`{"device_name":"Test Mac","return":"tab"}`))
	require.Equal(t, http.StatusOK, resp.StatusCode, body)
	var created map[string]any
	require.NoError(t, json.Unmarshal([]byte(body), &created))
	id := created["id"].(string)
	assert.Equal(t, "http://localhost/v2/compass/login/"+id+"/start", created["url"])
	assert.Regexp(t, `^[A-Z2-9]{3}-[A-Z2-9]{3}$`, created["user_code"])
	signInWithBrowser(t, base, created["url"].(string), "alice@example.com")
	_, body = a.call("GET", "/v2/compass/login/"+id, nil)
	assert.JSONEq(t, `{"status":"done","user":{"name":"Alice","email":"alice@example.com"}}`, body)

	// Now it syncs.
	initial := a.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID, bookmarkTypeID)
	require.NotNil(t, findByTag(initial, "bookmark_bar"))
	code, _ = a.quota(a.token(time.Now()))
	assert.Equal(t, http.StatusOK, code)

	// A device paired into the chain is signed in already (same chain).
	paired := newDevice(t, base, seed, "cache-guid-b")
	paired.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID)

	// A profile chain is linked.
	profile := newDevice(t, base, randomSeed(t), "cache-guid-p")
	resp, _ = a.call("POST", "/v2/compass/account/link", nil, oidclogin.LinkedTokenHeader, profile.token(time.Now()))
	assert.Equal(t, http.StatusNoContent, resp.StatusCode)
	profile.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID)

	// The admin commands.
	t.Setenv("COMPASS_SYNC_DB", dbPath)
	var out, errOut bytes.Buffer
	require.Equal(t, 0, server.Users([]string{"list"}, &out, &errOut), errOut.String())
	assert.Contains(t, out.String(), "alice@example.com")
	assert.Regexp(t, `alice@example\.com\s+Alice\s+fake-\S+\s+2\s`, out.String())
	out.Reset()
	require.Equal(t, 0, server.Chains([]string{"list"}, &out, &errOut), errOut.String())
	assert.Contains(t, out.String(), strings.ToLower(a.chainID()))
	assert.Contains(t, out.String(), "never (revalidated)")
	assert.Equal(t, 1, server.Users([]string{"revoke", "nobody@example.com"}, &out, &errOut))
	assert.Equal(t, 2, server.Users([]string{"revoke"}, &out, &errOut))
	out.Reset()
	require.Equal(t, 0, server.Users([]string{"revoke", "alice@example.com", "--delete-data"}, &out, &errOut),
		errOut.String())
	assert.Contains(t, out.String(), "2 chain(s) signed out")

	code, _ = a.post(getUpdatesMsg(), a.token(time.Now()))
	assert.Equal(t, http.StatusUnauthorized, code)
	_, body = a.call("GET", "/v2/compass/account", nil)
	assert.JSONEq(t, `{"login_required":true,"user":null,"expires":0}`, body)
	_, body = profile.call("GET", "/v2/compass/quota", nil)
	assert.JSONEq(t, loginRequired, body)
	// The data is gone and the chain disabled (for when it signs in again).
	db, err := datastore.NewSQLite(dbPath, datastore.SQLiteOptions{})
	require.NoError(t, err)
	defer db.Close()
	disabled, err := db.IsSyncChainDisabled(context.Background(), a.chainID())
	require.NoError(t, err)
	assert.True(t, disabled)

	// Restored, they can sign in again; then sign out.
	out.Reset()
	require.Equal(t, 0, server.Users([]string{"restore", "alice@example.com"}, &out, &errOut))
	assert.Contains(t, out.String(), "restored")
	resp, body = a.call("POST", "/v2/compass/login", []byte(`{"device_name":"Test Mac"}`))
	require.Equal(t, http.StatusOK, resp.StatusCode, body)
	require.NoError(t, json.Unmarshal([]byte(body), &created))
	signInWithBrowser(t, base, created["url"].(string), "alice@example.com")
	_, body = a.call("GET", "/v2/compass/account", nil)
	assert.Contains(t, body, `"login_required":false`)
	resp, _ = a.call("DELETE", "/v2/compass/account", nil)
	assert.Equal(t, http.StatusNoContent, resp.StatusCode)
	_, body = a.call("GET", "/v2/compass/account", nil)
	assert.Contains(t, body, `"login_required":true`)
}

// With sign-in off (the default), the new endpoints say so and nothing is
// gated (the rest of this package's tests run that way).
func TestOIDCOffByDefault(t *testing.T) {
	base := serverURL(t)
	a := newDevice(t, base, randomSeed(t), "cache-guid-a")
	_, body := a.call("GET", "/v2/compass/info", nil)
	assert.JSONEq(t, `{"login":null}`, body)
	_, body = a.call("GET", "/v2/compass/account", nil)
	assert.JSONEq(t, `{"login_required":false,"user":null,"expires":0}`, body)
	resp, body := a.call("POST", "/v2/compass/login", []byte(`{}`))
	assert.Equal(t, http.StatusNotFound, resp.StatusCode)
	assert.JSONEq(t, `{"error":"login_disabled"}`, body)
	a.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID)
	// A bad token's 401 is unchanged: plain text, no sign-in header.
	req, err := http.NewRequest("POST", base+"/v2/command/", nil)
	require.NoError(t, err)
	r, err := http.DefaultClient.Do(req)
	require.NoError(t, err)
	assert.Equal(t, http.StatusUnauthorized, r.StatusCode)
	assert.Equal(t, "Unauthorized\n", readAll(t, r))
	assert.Empty(t, r.Header.Get("X-Compass-Login"))
}
