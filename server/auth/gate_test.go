package auth_test

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/auth"
	"github.com/brave/go-sync/auth/authtest"
)

func TestAuthorizeSync(t *testing.T) {
	_, key, err := ed25519.GenerateKey(rand.Reader)
	require.NoError(t, err)
	token, id := authtest.TokenFor(key, time.Now().UnixMilli())
	req := httptest.NewRequest("GET", "/", nil)
	req.Header.Set("Authorization", "Bearer "+token)

	// No gate: Authorize itself.
	got, err := auth.AuthorizeSync(nil)(req)
	require.NoError(t, err)
	assert.Equal(t, strings.ToUpper(id), got)

	var asked string
	gate := func(_ context.Context, chain string) error {
		asked = chain
		return nil
	}
	got, err = auth.AuthorizeSync(gate)(req)
	require.NoError(t, err)
	assert.Equal(t, strings.ToUpper(id), got, "the ID as sent")
	assert.Equal(t, id, asked, "the gate gets it lower-case")

	_, err = auth.AuthorizeSync(func(context.Context, string) error { return auth.ErrLoginRequired })(req)
	require.ErrorIs(t, err, auth.ErrLoginRequired)
	rec := httptest.NewRecorder()
	assert.True(t, auth.WriteGateError(rec, err))
	assert.Equal(t, http.StatusUnauthorized, rec.Code)
	assert.Equal(t, "required", rec.Header().Get(auth.LoginHeader))
	assert.JSONEq(t, `{"error":"login_required"}`, rec.Body.String())

	_, err = auth.AuthorizeSync(func(context.Context, string) error { return errors.New("db down") })(req)
	require.ErrorIs(t, err, auth.ErrGateUnavailable)
	rec = httptest.NewRecorder()
	assert.True(t, auth.WriteGateError(rec, err))
	assert.Equal(t, http.StatusServiceUnavailable, rec.Code)

	// A bad token never reaches the gate, and isn't a gate error.
	_, err = auth.AuthorizeSync(gate)(httptest.NewRequest("GET", "/", nil))
	require.Error(t, err)
	assert.False(t, auth.WriteGateError(httptest.NewRecorder(), err))

	got, err = auth.AuthenticateToken(token)
	require.NoError(t, err)
	assert.Equal(t, strings.ToUpper(id), got)
	_, err = auth.AuthenticateToken("Bearer " + token)
	require.NoError(t, err)
	_, err = auth.AuthenticateToken("")
	require.Error(t, err)
}

func TestRejectRemovedDevices(t *testing.T) {
	_, key, err := ed25519.GenerateKey(rand.Reader)
	require.NoError(t, err)
	token, id := authtest.TokenFor(key, time.Now().UnixMilli())
	request := func(query string) *http.Request {
		req := httptest.NewRequest("GET", "/"+query, nil)
		req.Header.Set("Authorization", "Bearer "+token)
		return req
	}
	var asked []string
	authorize := auth.RejectRemovedDevices(auth.Authorize, func(_ context.Context, chain, guid string) (bool, error) {
		asked = append(asked, chain+" "+guid)
		if guid == "broken" {
			return false, errors.New("db down")
		}
		return guid == "gone", nil
	})

	got, err := authorize(request(""))
	require.NoError(t, err)
	assert.Equal(t, strings.ToUpper(id), got)
	assert.Empty(t, asked, "no client_id: not checked")

	got, err = authorize(request("?client_id=here"))
	require.NoError(t, err)
	assert.Equal(t, strings.ToUpper(id), got)
	assert.Equal(t, []string{strings.ToUpper(id) + " here"}, asked)

	got, err = authorize(request("?client_id=gone"))
	assert.Empty(t, got)
	require.ErrorIs(t, err, auth.ErrDeviceRemoved)
	rec := httptest.NewRecorder()
	assert.True(t, auth.WriteGateError(rec, err))
	assert.Equal(t, http.StatusForbidden, rec.Code)
	assert.Equal(t, "removed", rec.Header().Get(auth.DeviceHeader))
	assert.JSONEq(t, `{"error":"device_removed"}`, rec.Body.String())

	_, err = authorize(request("?client_id=broken"))
	require.ErrorIs(t, err, auth.ErrGateUnavailable)

	// A bad token is refused before the check.
	asked = nil
	_, err = authorize(httptest.NewRequest("GET", "/?client_id=gone", nil))
	require.Error(t, err)
	assert.Empty(t, asked)
	assert.False(t, auth.WriteGateError(httptest.NewRecorder(), err))
}

func TestAuthorizeRooms(t *testing.T) {
	_, key, err := ed25519.GenerateKey(rand.Reader)
	require.NoError(t, err)
	token, id := authtest.TokenFor(key, time.Now().UnixMilli())
	request := func(query string) *http.Request {
		req := httptest.NewRequest("GET", "/"+query, nil)
		req.Header.Set("Authorization", "Bearer "+token)
		return req
	}

	// No allowlist: listed.
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", "")
	got, listed, err := auth.AuthorizeRooms(nil, nil)(request(""))
	require.NoError(t, err)
	assert.Equal(t, strings.ToUpper(id), got)
	assert.True(t, listed)

	// Another chain listed: this one passes, not listed (Authorize refuses it).
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", strings.Repeat("ab", 32))
	_, err = auth.Authorize(request(""))
	require.Error(t, err)
	got, listed, err = auth.AuthorizeRooms(nil, nil)(request(""))
	require.NoError(t, err)
	assert.Equal(t, strings.ToUpper(id), got)
	assert.False(t, listed)
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", "ab, "+strings.ToUpper(id))
	_, listed, err = auth.AuthorizeRooms(nil, nil)(request(""))
	require.NoError(t, err)
	assert.True(t, listed)

	// The sign-in gate and removed devices still apply.
	_, _, err = auth.AuthorizeRooms(func(context.Context, string) error { return auth.ErrLoginRequired }, nil)(request(""))
	require.ErrorIs(t, err, auth.ErrLoginRequired)
	_, _, err = auth.AuthorizeRooms(func(context.Context, string) error { return errors.New("db") }, nil)(request(""))
	require.ErrorIs(t, err, auth.ErrGateUnavailable)
	removed := func(_ context.Context, _, guid string) (bool, error) { return guid == "gone", nil }
	_, _, err = auth.AuthorizeRooms(nil, removed)(request("?client_id=gone"))
	require.ErrorIs(t, err, auth.ErrDeviceRemoved)
	_, _, err = auth.AuthorizeRooms(nil, removed)(request("?client_id=here"))
	require.NoError(t, err)

	// Blocked chains and bad tokens are refused.
	t.Setenv("BLOCKED_CLIENT_IDS", id)
	_, _, err = auth.AuthorizeRooms(nil, nil)(request(""))
	require.Error(t, err)
	_, _, err = auth.AuthorizeRooms(nil, nil)(httptest.NewRequest("GET", "/", nil))
	require.Error(t, err)
	assert.False(t, auth.WriteGateError(httptest.NewRecorder(), err))
}

// A private server lets in chains linked from a listed one (profile
// accounts, sync.md "Profiles"), unless that root is blocked.
func TestLinkedChainsPassTheAllowlist(t *testing.T) {
	_, homeKey, err := ed25519.GenerateKey(rand.Reader)
	require.NoError(t, err)
	_, workKey, err := ed25519.GenerateKey(rand.Reader)
	require.NoError(t, err)
	_, home := authtest.TokenFor(homeKey, time.Now().UnixMilli())
	workToken, work := authtest.TokenFor(workKey, time.Now().UnixMilli())
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", home)
	t.Setenv("BLOCKED_CLIENT_IDS", "")
	t.Cleanup(func() { auth.SetLinkedRoot(nil) })

	_, err = auth.Authenticate(workToken)
	require.ErrorContains(t, err, "chain not allowed")
	links := map[string]string{}
	auth.SetLinkedRoot(func(_ context.Context, id string) (string, error) { return links[id], nil })
	_, err = auth.Authenticate(workToken)
	require.Error(t, err)

	links[strings.ToLower(work)] = strings.ToLower(home)
	got, err := auth.Authenticate(workToken)
	require.NoError(t, err)
	assert.Equal(t, strings.ToLower(work), strings.ToLower(got))
	assert.True(t, auth.ChainAllowed(context.Background(), work))

	// The root no longer listed, or blocked: refused again.
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", strings.Repeat("ab", 32))
	_, err = auth.Authenticate(workToken)
	require.Error(t, err)
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", home)
	t.Setenv("BLOCKED_CLIENT_IDS", home)
	_, err = auth.Authenticate(workToken)
	require.Error(t, err)
	// A failed lookup isn't a link.
	t.Setenv("BLOCKED_CLIENT_IDS", "")
	auth.SetLinkedRoot(func(context.Context, string) (string, error) { return "", errors.New("db") })
	_, err = auth.Authenticate(workToken)
	require.Error(t, err)
}
