package server_test

import (
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"github.com/go-chi/chi/v5"
	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/server"
)

var (
	mux       *chi.Mux
	serverCtx context.Context
)

func TestMain(m *testing.M) {
	dir, err := os.MkdirTemp("", "compass-sync-server-test")
	if err != nil {
		panic(err)
	}
	db, err := datastore.NewSQLite(filepath.Join(dir, "test.db"), datastore.SQLiteOptions{})
	if err != nil {
		panic(err)
	}
	cfg, err := server.LoadConfig()
	if err != nil {
		panic(err)
	}
	testCtx, logger := server.SetupLogger(context.Background())
	serverCtx, mux, _, err = server.SetupRouter(testCtx, logger, cfg, db) //nolint:fatcontext // TestMain assigns shared context once
	if err != nil {
		panic(err)
	}
	code := m.Run()
	db.Close()
	os.RemoveAll(dir)
	os.Exit(code)
}

func TestPing(t *testing.T) {
	req, err := http.NewRequest("GET", "/", nil)
	require.NoError(t, err)

	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, req.WithContext(serverCtx))
	assert.Equal(t, http.StatusOK, rr.Code)

	expected := "."
	actual, err := io.ReadAll(rr.Result().Body)
	require.NoError(t, err)
	assert.Equal(t, expected, string(actual))
}

func TestCommand(t *testing.T) {
	req, err := http.NewRequest("POST", "/v2/command/", nil)
	require.NoError(t, err)

	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, req.WithContext(serverCtx))
	assert.Equal(t, http.StatusUnauthorized, rr.Code)
	assert.NotEmpty(t, rr.Result().Header.Get("Sane-Time-Millis"))
}

func TestHealthz(t *testing.T) {
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, httptest.NewRequest("GET", "/healthz", nil))
	assert.Equal(t, http.StatusOK, rr.Code)
	assert.Equal(t, "ok\n", rr.Body.String())
}

func TestPairingIsMountedOutsideCommandAuth(t *testing.T) {
	// Opening needs a sync token...
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, httptest.NewRequest("POST", "/v2/compass/pair", nil))
	assert.Equal(t, http.StatusUnauthorized, rr.Code)
	// ...joining doesn't (an unknown slot is a 404, not a 401).
	rr = httptest.NewRecorder()
	mux.ServeHTTP(rr, httptest.NewRequest("POST", "/v2/compass/pair/42/join", nil))
	assert.Equal(t, http.StatusNotFound, rr.Code)
}

func TestMetricsOffByDefault(t *testing.T) {
	rr := httptest.NewRecorder()
	mux.ServeHTTP(rr, httptest.NewRequest("GET", "/metrics", nil))
	assert.Equal(t, http.StatusNotFound, rr.Code)
}
