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
