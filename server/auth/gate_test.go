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
