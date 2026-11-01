package quota_test

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/quota"
)

type fakeStore struct {
	counts   datastore.ClientItemCounts
	bytes    int64
	full     datastore.QuotaFull
	since    time.Time
	disabled bool
}

func (f *fakeStore) GetClientItemCount(_ context.Context, id string) (*datastore.ClientItemCounts, error) {
	c := f.counts
	c.ClientID = id
	return &c, nil
}

func (f *fakeStore) ChainBytes(context.Context, string) (int64, error) { return f.bytes, nil }

func (f *fakeStore) QuotaState(context.Context, string) (datastore.QuotaFull, time.Time, error) {
	return f.full, f.since, nil
}

func (f *fakeStore) IsSyncChainDisabled(context.Context, string) (bool, error) {
	return f.disabled, nil
}

func newHandler(store *fakeStore) *quota.Handler {
	return quota.New(quota.Config{
		Store: store,
		Authorize: func(r *http.Request) (string, error) {
			if r.Header.Get("Authorization") != "Bearer good" {
				return "", errors.New("bad token")
			}
			return "CHAIN", nil
		},
		Limits: func() quota.Limits {
			return quota.Limits{MaxEntities: 100, MaxBytes: 1 << 20, MaxHistory: 10, HistoryDays: 14}
		},
	})
}

func get(t *testing.T, h http.Handler, method, token string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(method, "/", nil)
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	w := httptest.NewRecorder()
	h.ServeHTTP(w, req)
	return w
}

func decode(t *testing.T, w *httptest.ResponseRecorder) quota.Status {
	t.Helper()
	require.Equal(t, http.StatusOK, w.Code)
	assert.Equal(t, "application/json", w.Header().Get("Content-Type"))
	var st quota.Status
	require.NoError(t, json.Unmarshal(w.Body.Bytes(), &st))
	return st
}

func TestStatusRoom(t *testing.T) {
	store := &fakeStore{
		counts: datastore.ClientItemCounts{ItemCount: 40, HistoryItemCountPeriod2: 3, HistoryItemCountPeriod4: 2},
		bytes:  1000,
	}
	st := decode(t, get(t, newHandler(store), http.MethodGet, "good"))
	assert.Equal(t, quota.Status{
		Entities: 45, MaxEntities: 100, Bytes: 1000, MaxBytes: 1 << 20,
		History: 5, MaxHistory: 10, HistoryDays: 14, Full: []string{}, FullSince: 0,
	}, st)
}

func TestStatusFull(t *testing.T) {
	since := time.Unix(1_800_000_000, 0)
	// Refusals on record, and live counts at the limits.
	store := &fakeStore{
		counts: datastore.ClientItemCounts{ItemCount: 90, HistoryItemCountPeriod1: 10},
		bytes:  1000,
		full:   datastore.QuotaBytes,
		since:  since,
	}
	st := decode(t, get(t, newHandler(store), http.MethodGet, "good"))
	assert.Equal(t, []string{"entities", "bytes", "history"}, st.Full)
	assert.Equal(t, since.Unix(), st.FullSince)

	// Only the history quota on record.
	store = &fakeStore{full: datastore.QuotaHistory, since: since}
	st = decode(t, get(t, newHandler(store), http.MethodGet, "good"))
	assert.Equal(t, []string{"history"}, st.Full)

	// Bytes within a large entry of the limit count as full.
	store = &fakeStore{bytes: 1<<20 - 1000}
	st = decode(t, get(t, newHandler(store), http.MethodGet, "good"))
	assert.Equal(t, []string{"bytes"}, st.Full)
}

func TestStatusRefused(t *testing.T) {
	h := newHandler(&fakeStore{})
	assert.Equal(t, http.StatusUnauthorized, get(t, h, http.MethodGet, "").Code)
	assert.Equal(t, http.StatusUnauthorized, get(t, h, http.MethodGet, "bad").Code)
	assert.Equal(t, http.StatusMethodNotAllowed, get(t, h, http.MethodPost, "good").Code)
	assert.Equal(t, http.StatusForbidden,
		get(t, newHandler(&fakeStore{disabled: true}), http.MethodGet, "good").Code)
}
