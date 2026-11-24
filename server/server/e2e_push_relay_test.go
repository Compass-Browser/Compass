package server_test

// A server without APNs or FCM keys pushes through the Compass push relay
// (push/relay.go, COMPASS_SYNC_PUSH_RELAY): registrations are sealed there
// and only handles are kept; pushes are asked of it. A fake relay here.

import (
	"context"
	"encoding/json"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/push"
	"github.com/brave/go-sync/server"
)

type fakeRelay struct {
	srv    *httptest.Server
	mu     sync.Mutex
	pushes []map[string]string
	seals  []map[string]string
	gone   map[string]bool
	down   bool
}

func newFakeRelay(t *testing.T) *fakeRelay {
	f := &fakeRelay{gone: map[string]bool{}}
	f.srv = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		raw, _ := io.ReadAll(r.Body)
		var body map[string]string
		_ = json.Unmarshal(raw, &body)
		f.mu.Lock()
		defer f.mu.Unlock()
		w.Header().Set("Content-Type", "application/json")
		if f.down {
			w.WriteHeader(http.StatusBadGateway)
			return
		}
		switch r.URL.Path {
		case "/v1/seal":
			f.seals = append(f.seals, body)
			_ = json.NewEncoder(w).Encode(map[string]string{"handle": "h." + body["platform"] + "." + body["token"]})
		case "/v1/push":
			f.pushes = append(f.pushes, body)
			if f.gone[body["handle"]] {
				w.WriteHeader(http.StatusGone)
				_, _ = w.Write([]byte(`{"error":"gone"}`))
				return
			}
			_, _ = w.Write([]byte(`{"result":"sent"}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
	}))
	t.Cleanup(f.srv.Close)
	return f
}

// took returns the pushes since the last call, as "type handle", once
// |n| arrived (or after a moment for none).
func (f *fakeRelay) took(t *testing.T, n int) []string {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	if n == 0 {
		deadline = time.Now().Add(300 * time.Millisecond)
	}
	for {
		f.mu.Lock()
		got := len(f.pushes)
		f.mu.Unlock()
		if (n > 0 && got >= n) || time.Now().After(deadline) {
			break
		}
		time.Sleep(20 * time.Millisecond)
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	var out []string
	for _, p := range f.pushes {
		out = append(out, p["type"]+" "+p["handle"])
	}
	f.pushes = nil
	return out
}

func relayServer(t *testing.T) (string, *fakeRelay, *datastore.SQLite) {
	relay := newFakeRelay(t)
	db, err := datastore.NewSQLite(filepath.Join(t.TempDir(), "push.db"), datastore.SQLiteOptions{})
	require.NoError(t, err)
	t.Cleanup(func() { db.Close() })
	cfg, err := server.LoadConfig()
	require.NoError(t, err)
	cfg.Push.APNs = push.APNsConfig{}
	cfg.Push.FCM = push.FCMConfig{}
	cfg.Push.Relay = relay.srv.URL
	cfg.Push.PublicHost = "sync.example.org"
	cfg.Push.BackgroundInterval = backgroundInterval
	ctx, logger := server.SetupLogger(context.Background())
	ctx, r, hub, err := server.SetupRouter(ctx, logger, cfg, db)
	require.NoError(t, err)
	t.Cleanup(hub.Close)
	srv := httptest.NewUnstartedServer(r)
	srv.Config.BaseContext = func(_ net.Listener) context.Context { return ctx }
	srv.Start()
	t.Cleanup(srv.Close)
	return srv.URL, relay, db
}

func TestEndToEndPushThroughRelay(t *testing.T) {
	url, relay, db := relayServer(t)
	seed := randomSeed(t)
	mac := newDevice(t, url, seed, "mac-guid")
	phone := newDevice(t, url, seed, "phone-guid")
	droid := newDevice(t, url, seed, "droid-guid")
	fcmToken := "fcm:" + strings.Repeat("x", 60)

	require.Equal(t, http.StatusNoContent, phone.register("POST", phone.token(time.Now()), registration(phoneToken, "production")))
	require.Equal(t, http.StatusNoContent, droid.register("POST", droid.token(time.Now()),
		`{"platform":"fcm","token":"`+fcmToken+`","topic":"it.cipi1965.compass"}`))

	// Sealed for this server's public name; only the handle is kept.
	relay.mu.Lock()
	require.Len(t, relay.seals, 2)
	assert.Equal(t, map[string]string{"server": "sync.example.org", "platform": "apns", "token": phoneToken,
		"environment": "production", "topic": "it.cipi1965.compass"}, relay.seals[0])
	assert.Equal(t, "fcm", relay.seals[1]["platform"])
	relay.mu.Unlock()
	toks, err := db.PushTokens(context.Background(), phone.chainID())
	require.NoError(t, err)
	for _, tok := range toks {
		assert.True(t, strings.HasPrefix(tok.Token, "h."), tok.Token)
		assert.Equal(t, "sync.example.org", tok.Topic)
	}

	// A sent tab: both phones, through the relay.
	mac.commit(sentTab("local-tab-1", 0))
	assert.ElementsMatch(t, []string{"send-tab h.apns." + phoneToken, "send-tab h.fcm." + fcmToken}, relay.took(t, 2))

	// A password: the iPhone syncs in the background; Android doesn't.
	mac.commit(password("pw-1"))
	assert.Equal(t, []string{"sync h.apns." + phoneToken}, relay.took(t, 1))

	// A handle the relay calls gone is dropped.
	relay.mu.Lock()
	relay.gone["h.apns."+phoneToken] = true
	relay.mu.Unlock()
	mac.commit(sentTab("local-tab-2", 0))
	relay.took(t, 2)
	require.Eventually(t, func() bool {
		toks, err := db.PushTokens(context.Background(), phone.chainID())
		return err == nil && len(toks) == 1 && toks[0].CacheGUID == "droid-guid"
	}, 5*time.Second, 20*time.Millisecond)

	// The relay down: the device is told to register again later.
	relay.mu.Lock()
	relay.down = true
	relay.mu.Unlock()
	assert.Equal(t, http.StatusServiceUnavailable, phone.register("POST", phone.token(time.Now()), registration(phoneToken, "production")))
}
