package server_test

// Sent tabs wake the chain's phones (docs/design/send-tab.md, "Delivery"):
// devices register APNs tokens on /v2/compass/push; a commit that adds a
// SEND_TAB_TO_SELF entity pushes to every other registered token, through a
// fake APNs (HTTP/2, TLS) here.

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/push"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
	"github.com/brave/go-sync/server"
)

type apnsRequest struct {
	path    string
	header  http.Header
	payload map[string]any
	proto   int
}

type fakeAPNs struct {
	srv  *httptest.Server
	pub  *ecdsa.PublicKey
	mu   sync.Mutex
	reqs []apnsRequest
	got  chan struct{}
	// answer by device token: status and reason; default 200.
	answer map[string][2]string
}

func newFakeAPNs(t *testing.T, pub *ecdsa.PublicKey) *fakeAPNs {
	f := &fakeAPNs{pub: pub, got: make(chan struct{}, 100), answer: map[string][2]string{}}
	f.srv = httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		raw, _ := io.ReadAll(r.Body)
		var payload map[string]any
		_ = json.Unmarshal(raw, &payload)
		f.mu.Lock()
		f.reqs = append(f.reqs, apnsRequest{path: r.URL.Path, header: r.Header.Clone(), payload: payload, proto: r.ProtoMajor})
		ans, ok := f.answer[strings.TrimPrefix(r.URL.Path, "/3/device/")]
		f.mu.Unlock()
		defer func() { f.got <- struct{}{} }()
		if !push.VerifyToken(strings.TrimPrefix(r.Header.Get("Authorization"), "bearer "), f.pub) {
			w.WriteHeader(http.StatusForbidden)
			_, _ = w.Write([]byte(`{"reason":"InvalidProviderToken"}`))
			return
		}
		if ok {
			w.WriteHeader(map[string]int{"410": 410, "400": 400}[ans[0]])
			_, _ = w.Write([]byte(`{"reason":"` + ans[1] + `"}`))
			return
		}
		w.WriteHeader(http.StatusOK)
	}))
	f.srv.EnableHTTP2 = true
	f.srv.StartTLS()
	t.Cleanup(f.srv.Close)
	return f
}

func (f *fakeAPNs) wait(t *testing.T, n int) []apnsRequest {
	t.Helper()
	for range n {
		select {
		case <-f.got:
		case <-time.After(5 * time.Second):
			t.Fatalf("APNs got fewer than %d pushes", n)
		}
	}
	select {
	case <-f.got:
		t.Fatalf("APNs got more than %d pushes", n)
	case <-time.After(200 * time.Millisecond):
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	out := f.reqs
	f.reqs = nil
	return out
}

// pushServer is a server with APNs pointed at a fake.
func pushServer(t *testing.T) (string, *fakeAPNs, *datastore.SQLite) {
	dir := t.TempDir()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	require.NoError(t, err)
	der, err := x509.MarshalPKCS8PrivateKey(key)
	require.NoError(t, err)
	keyPath := filepath.Join(dir, "AuthKey_TEST.p8")
	require.NoError(t, os.WriteFile(keyPath, pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der}), 0o600))
	apns := newFakeAPNs(t, &key.PublicKey)

	db, err := datastore.NewSQLite(filepath.Join(dir, "push.db"), datastore.SQLiteOptions{})
	require.NoError(t, err)
	t.Cleanup(func() { db.Close() })
	cfg, err := server.LoadConfig()
	require.NoError(t, err)
	cfg.Push.APNs = push.APNsConfig{KeyPath: keyPath, KeyID: "KEY123", TeamID: "TEAM123",
		Host: apns.srv.URL, HTTPClient: apns.srv.Client()}
	ctx, logger := server.SetupLogger(context.Background())
	ctx, r, hub, err := server.SetupRouter(ctx, logger, cfg, db)
	require.NoError(t, err)
	t.Cleanup(hub.Close)
	srv := httptest.NewUnstartedServer(r)
	srv.Config.BaseContext = func(_ net.Listener) context.Context { return ctx }
	srv.Start()
	t.Cleanup(srv.Close)
	return srv.URL, apns, db
}

func (d *device) register(method, token, body string) int {
	d.t.Helper()
	req, err := http.NewRequest(method, d.url+"/v2/compass/push?client_id="+d.cacheGUID, strings.NewReader(body))
	require.NoError(d.t, err)
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	require.NoError(d.t, err)
	resp.Body.Close()
	return resp.StatusCode
}

func registration(token, env string) string {
	return `{"token":"` + token + `","environment":"` + env + `","topic":"it.cipi1965.compass"}`
}

// sentTab is an encrypted SEND_TAB_TO_SELF entity, as Chromium commits it.
func sentTab(id string, version int64) *sync_pb.SyncEntity {
	enc := &sync_pb.EncryptedData{KeyName: ptr.String("k"), Blob: ptr.String("opaque")}
	spec := &sync_pb.EntitySpecifics{Encrypted: enc,
		SpecificsVariant: &sync_pb.EntitySpecifics_SendTabToSelf{SendTabToSelf: &sync_pb.SendTabToSelfSpecifics{}}}
	return &sync_pb.SyncEntity{IdString: ptr.String(id), Version: ptr.Int64(version),
		ClientTagHash: ptr.String("tag-" + id), Name: ptr.String("encrypted"),
		Deleted: ptr.Bool(false), Specifics: spec}
}

const (
	phoneToken  = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
	phone2Token = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	macToken    = "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
)

func TestEndToEndSendTabPush(t *testing.T) {
	url, apns, db := pushServer(t)
	seed := randomSeed(t)
	mac := newDevice(t, url, seed, "mac-guid")
	phone := newDevice(t, url, seed, "phone-guid")
	phone2 := newDevice(t, url, seed, "phone2-guid")
	stranger := newDevice(t, url, randomSeed(t), "stranger-guid")

	// Registrations: the phones, the Mac (a committer is skipped), and
	// another chain's device (never woken by this chain).
	require.Equal(t, http.StatusNoContent, phone.register("POST", phone.token(time.Now()), registration(phoneToken, "sandbox")))
	require.Equal(t, http.StatusNoContent, phone2.register("POST", phone2.token(time.Now()), registration(phone2Token, "production")))
	require.Equal(t, http.StatusNoContent, mac.register("POST", mac.token(time.Now()), registration(macToken, "production")))
	require.Equal(t, http.StatusNoContent, stranger.register("POST", stranger.token(time.Now()), registration("dd"+phoneToken[2:], "production")))

	// The Mac sends a tab: both phones are woken, once each.
	rsps := mac.commit(sentTab("local-tab-1", 0))
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, rsps[0].GetResponseType())
	reqs := apns.wait(t, 2)
	paths := []string{reqs[0].path, reqs[1].path}
	assert.ElementsMatch(t, []string{"/3/device/" + phoneToken, "/3/device/" + phone2Token}, paths)
	for _, r := range reqs {
		assert.Equal(t, 2, r.proto, "APNs needs HTTP/2")
		assert.Equal(t, "alert", r.header.Get("apns-push-type"))
		assert.Equal(t, "10", r.header.Get("apns-priority"))
		assert.Equal(t, "it.cipi1965.compass", r.header.Get("apns-topic"))
		assert.NotEmpty(t, r.header.Get("apns-expiration"))
		aps := r.payload["aps"].(map[string]any)
		assert.Equal(t, float64(1), aps["mutable-content"])
		assert.Equal(t, "sent-tab", aps["thread-id"])
		assert.Equal(t, "New tab", aps["alert"].(map[string]any)["title"])
		assert.Equal(t, "send-tab", r.payload["compass"].(map[string]any)["type"])
	}

	// The phone marks it opened (an update, not a new entity): no push.
	phone.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, 601980)
	upd := phone.commit(sentTab(rsps[0].GetIdString(), rsps[0].GetVersion()))
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, upd[0].GetResponseType())
	// Nor does another type's new entity.
	mac.commit(&sync_pb.SyncEntity{IdString: ptr.String("local-f"), Version: ptr.Int64(0),
		ClientTagHash: ptr.String("f-tag"), Specifics: func() *sync_pb.EntitySpecifics { s, _ := futureSpecifics(t, []byte("x")); return s }()})
	apns.wait(t, 0)

	// A phone APNs says is gone is dropped; one with a bad token too.
	apns.mu.Lock()
	apns.answer[phoneToken] = [2]string{"410", "Unregistered"}
	apns.answer[phone2Token] = [2]string{"400", "BadDeviceToken"}
	apns.mu.Unlock()
	mac.commit(sentTab("local-tab-2", 0))
	apns.wait(t, 2)
	require.Eventually(t, func() bool {
		toks, err := db.PushTokens(context.Background(), mac.chainID())
		return err == nil && len(toks) == 1 && toks[0].CacheGUID == "mac-guid"
	}, 5*time.Second, 20*time.Millisecond)

	// The phone registers again, then leaves the chain: nothing is sent.
	require.Equal(t, http.StatusNoContent, phone.register("POST", phone.token(time.Now()), registration(phoneToken, "sandbox")))
	require.Equal(t, http.StatusNoContent, phone.register("DELETE", phone.token(time.Now()), ""))
	mac.commit(sentTab("local-tab-3", 0))
	apns.wait(t, 0)
}

func TestPushRegistrationValidation(t *testing.T) {
	url, _, db := pushServer(t)
	d := newDevice(t, url, randomSeed(t), "guid")
	tok := d.token(time.Now())
	assert.Equal(t, http.StatusUnauthorized, d.register("POST", "nope", registration(phoneToken, "sandbox")))
	assert.Equal(t, http.StatusUnauthorized, d.register("POST", d.token(time.Now().Add(-25*time.Hour)), registration(phoneToken, "sandbox")))
	assert.Equal(t, http.StatusBadRequest, d.register("POST", tok, "not json"))
	assert.Equal(t, http.StatusBadRequest, d.register("POST", tok, registration("xyz", "sandbox")))
	assert.Equal(t, http.StatusBadRequest, d.register("POST", tok, registration(phoneToken, "staging")))
	assert.Equal(t, http.StatusBadRequest, d.register("POST", tok,
		`{"token":"`+phoneToken+`","environment":"sandbox","topic":"com.example.other"}`))
	noGUID := newDevice(t, url, randomSeed(t), "")
	assert.Equal(t, http.StatusBadRequest, noGUID.register("POST", noGUID.token(time.Now()), registration(phoneToken, "sandbox")))
	req, _ := http.NewRequest("GET", url+"/v2/compass/push?client_id=guid", nil)
	req.Header.Set("Authorization", "Bearer "+tok)
	resp, err := http.DefaultClient.Do(req)
	require.NoError(t, err)
	resp.Body.Close()
	assert.Equal(t, http.StatusMethodNotAllowed, resp.StatusCode)

	// Upper-case hex is stored lower-case; registering again replaces.
	assert.Equal(t, http.StatusNoContent, d.register("POST", tok, registration(strings.ToUpper(phoneToken), "sandbox")))
	assert.Equal(t, http.StatusNoContent, d.register("POST", tok, registration(phone2Token, "production")))
	toks, err := db.PushTokens(context.Background(), d.chainID())
	require.NoError(t, err)
	require.Len(t, toks, 1)
	assert.Equal(t, datastore.PushToken{CacheGUID: "guid", Token: phone2Token, Environment: "production",
		Topic: "it.cipi1965.compass"}, toks[0])
}
