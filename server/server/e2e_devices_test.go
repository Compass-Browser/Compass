package server_test

// Removing a device from its chain end to end (sync.md, "Identity",
// "Removing a device"): DELETE /v2/compass/devices/{cache_guid} deletes the
// device's DEVICE_INFO entry (the others get the tombstone), drops its push
// registration and refuses its cache GUID from then on.

import (
	"bytes"
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
	"github.com/brave/go-sync/server"
)

const deviceInfoTypeID = datastore.DeviceInfoTypeID

// devicesServer is a server on its own database; returns its URL, the
// database's path and the database.
func devicesServer(t *testing.T) (string, string, *datastore.SQLite) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "devices.db")
	db, err := datastore.NewSQLite(path, datastore.SQLiteOptions{})
	require.NoError(t, err)
	t.Cleanup(func() { db.Close() })
	cfg, err := server.LoadConfig()
	require.NoError(t, err)
	ctx, logger := server.SetupLogger(context.Background())
	ctx, r, hub, err := server.SetupRouter(ctx, logger, cfg, db)
	require.NoError(t, err)
	srv := httptest.NewUnstartedServer(r)
	srv.Config.BaseContext = func(_ net.Listener) context.Context { return ctx }
	srv.Start()
	t.Cleanup(func() { hub.Close(); srv.Close() })
	return srv.URL, path, db
}

func deviceInfoTagHash(guid string) string {
	return datastore.ClientTagHash(deviceInfoTypeID, datastore.DeviceInfoTag(guid))
}

// joinChain is a device's first sync: NEW_CLIENT, then its DEVICE_INFO.
func (d *device) joinChain(name string) {
	d.t.Helper()
	d.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID, deviceInfoTypeID)
	rsp := d.commit(&sync_pb.SyncEntity{
		IdString: ptr.String("local-device-info"), Version: ptr.Int64(0),
		Name: ptr.String(name), ClientTagHash: ptr.String(deviceInfoTagHash(d.cacheGUID)),
		Specifics: &sync_pb.EntitySpecifics{SpecificsVariant: &sync_pb.EntitySpecifics_DeviceInfo{
			DeviceInfo: &sync_pb.DeviceInfoSpecifics{
				CacheGuid: ptr.String(d.cacheGUID), ClientName: ptr.String(name),
				OsType:               sync_pb.SyncEnums_OS_TYPE_MAC.Enum(),
				LastUpdatedTimestamp: ptr.Int64(time.Now().UnixMilli()),
			}}},
	})
	require.Equal(d.t, sync_pb.CommitResponse_SUCCESS, rsp[0].GetResponseType(), rsp[0].GetErrorMessage())
}

func (d *device) removeDevice(guid, self string) (*http.Response, string) {
	d.t.Helper()
	q := ""
	if self != "" {
		q = "?client_id=" + url.QueryEscape(self)
	}
	return d.call(http.MethodDelete, "/v2/compass/devices/"+url.PathEscape(guid)+q, nil)
}

// liveDevices returns the live DEVICE_INFO cache GUIDs among |entries|, and
// the tag hashes of the deleted ones.
func liveDevices(entries []*sync_pb.SyncEntity) (live, deleted []string) {
	for _, e := range entries {
		if e.GetDeleted() {
			deleted = append(deleted, e.GetClientTagHash())
			continue
		}
		live = append(live, e.GetSpecifics().GetDeviceInfo().GetCacheGuid())
	}
	return live, deleted
}

// assertRemoved checks every per-device endpoint refuses |d|.
func assertRemoved(t *testing.T, d *device) {
	t.Helper()
	code, _ := d.post(getUpdatesMsg(), d.token(time.Now()))
	assert.Equal(t, http.StatusForbidden, code, "command")
	q := "?client_id=" + url.QueryEscape(d.cacheGUID)
	for _, ep := range []struct{ method, path string }{
		{"POST", "/v2/command/" + q},
		{"GET", "/v2/compass/events" + q},
		{"POST", "/v2/compass/push" + q},
		{"DELETE", "/v2/compass/push" + q},
		{"GET", "/v2/compass/quota" + q},
		{"DELETE", "/v2/compass/devices/someone" + q},
	} {
		resp, body := d.call(ep.method, ep.path, nil)
		assert.Equal(t, http.StatusForbidden, resp.StatusCode, ep.path)
		assert.JSONEq(t, `{"error":"device_removed"}`, body, ep.path)
		assert.Equal(t, "removed", resp.Header.Get("X-Compass-Device"), ep.path)
	}
}

func TestEndToEndRemoveDevice(t *testing.T) {
	base, _, db := devicesServer(t)
	ctx := context.Background()
	seed := randomSeed(t)
	a := newDevice(t, base, seed, "guid-a")
	b := newDevice(t, base, seed, "guid-b")
	// Chromium's cache GUIDs are base64: "/" goes percent-encoded in the path.
	c := newDevice(t, base, seed, "c/Xy9=")
	a.joinChain("Mac")
	b.joinChain("iPhone")
	c.joinChain("Old laptop")
	delete(b.tokens, deviceInfoTypeID) // B's own join read part of it.
	live, _ := liveDevices(b.getUpdates(sync_pb.SyncEnums_PERIODIC, deviceInfoTypeID))
	assert.ElementsMatch(t, []string{"guid-a", "guid-b", "c/Xy9="}, live)
	_, before := a.quota(a.token(time.Now()))

	// C has a push registration and an open change stream; B listens too.
	require.Equal(t, http.StatusNoContent, c.register("POST", c.token(time.Now()), registration(phoneToken, "sandbox")))
	require.Equal(t, http.StatusNoContent, b.register("POST", b.token(time.Now()), registration("ee"+phoneToken[2:], "sandbox")))
	sc, _ := c.subscribe(c.token(time.Now()))
	sb, _ := b.subscribe(b.token(time.Now()))

	resp, body := a.removeDevice("c/Xy9=", "guid-a")
	require.Equal(t, http.StatusNoContent, resp.StatusCode, body)

	// B hears of it and gets the tombstone, also on a poll (the type's
	// cached mtime moved).
	sb.expect("event: changed", `data: {"types":[154522]}`, "")
	live, deleted := liveDevices(b.getUpdates(sync_pb.SyncEnums_PERIODIC, deviceInfoTypeID))
	assert.Empty(t, live)
	assert.Equal(t, []string{deviceInfoTagHash("c/Xy9=")}, deleted)
	// A device joining now gets A and B only.
	fresh := newDevice(t, base, seed, "guid-d")
	live, _ = liveDevices(fresh.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, deviceInfoTypeID))
	assert.ElementsMatch(t, []string{"guid-a", "guid-b"}, live)

	// C's stream ended; C is refused everywhere; its push token is gone.
	for l := range sc.lines {
		t.Fatalf("unexpected line %q", l)
	}
	assertRemoved(t, c)
	toks, err := db.PushTokens(ctx, a.chainID())
	require.NoError(t, err)
	require.Len(t, toks, 1)
	assert.Equal(t, "guid-b", toks[0].CacheGUID)

	// The chain counts one entity less.
	_, after := a.quota(a.token(time.Now()))
	assert.Equal(t, before.Entities-1, after.Entities)
	// Without a client_id, C's token still reads the chain's quota (the
	// gate is per device, and its key is the chain's).
	code, _ := c.quota(c.token(time.Now()))
	assert.Equal(t, http.StatusOK, code)

	// Idempotent; a GUID with no entry is recorded too.
	resp, _ = a.removeDevice("c/Xy9=", "guid-a")
	assert.Equal(t, http.StatusNoContent, resp.StatusCode)
	resp, _ = b.removeDevice("never-seen", "guid-b")
	assert.Equal(t, http.StatusNoContent, resp.StatusCode)
	gone, err := db.IsDeviceRemoved(ctx, a.chainID(), "never-seen")
	require.NoError(t, err)
	assert.True(t, gone)

	// The old installation rejoins as a new device (a new cache GUID).
	c2 := newDevice(t, base, seed, "guid-c2")
	c2.joinChain("Old laptop")
	live, _ = liveDevices(b.getUpdates(sync_pb.SyncEnums_PERIODIC, deviceInfoTypeID))
	assert.Equal(t, []string{"guid-c2"}, live)
	a.getUpdates(sync_pb.SyncEnums_PERIODIC, bookmarkTypeID)
}

func TestRemoveDeviceErrors(t *testing.T) {
	base, _, _ := devicesServer(t)
	seed := randomSeed(t)
	a := newDevice(t, base, seed, "guid-a")
	b := newDevice(t, base, seed, "guid-b")
	a.joinChain("Mac")
	b.joinChain("Phone")

	resp, body := a.removeDevice("guid-a", "guid-a")
	assert.Equal(t, http.StatusConflict, resp.StatusCode)
	assert.JSONEq(t, `{"error":"this_device"}`, body)
	for _, bad := range []struct{ guid, self string }{
		{strings.Repeat("x", 129), "guid-a"},
		{"guid-b", ""},
		{"guid-b", strings.Repeat("x", 129)},
	} {
		resp, body = a.removeDevice(bad.guid, bad.self)
		assert.Equal(t, http.StatusBadRequest, resp.StatusCode, bad)
		assert.JSONEq(t, `{"error":"bad_request"}`, body)
	}
	resp, body = a.call(http.MethodDelete, "/v2/compass/devices/?client_id=guid-a", nil)
	assert.Equal(t, http.StatusBadRequest, resp.StatusCode)
	assert.JSONEq(t, `{"error":"bad_request"}`, body)

	// No token, a bad token: 401.
	req, err := http.NewRequest(http.MethodDelete, base+"/v2/compass/devices/guid-b?client_id=guid-a", nil)
	require.NoError(t, err)
	r, err := http.DefaultClient.Do(req)
	require.NoError(t, err)
	r.Body.Close()
	assert.Equal(t, http.StatusUnauthorized, r.StatusCode)
	resp, _ = a.call(http.MethodDelete, "/v2/compass/devices/guid-b?client_id=guid-a", nil,
		"Authorization", "Bearer "+a.token(time.Now().Add(-25*time.Hour)))
	assert.Equal(t, http.StatusUnauthorized, resp.StatusCode)

	// Another chain can't touch this one: its removal is its own.
	other := newDevice(t, base, randomSeed(t), "guid-x")
	other.joinChain("Stranger")
	resp, _ = other.removeDevice("guid-b", "guid-x")
	assert.Equal(t, http.StatusNoContent, resp.StatusCode)
	live, _ := liveDevices(a.getUpdates(sync_pb.SyncEnums_PERIODIC, deviceInfoTypeID))
	assert.ElementsMatch(t, []string{"guid-a", "guid-b"}, live)
	b.getUpdates(sync_pb.SyncEnums_PERIODIC, deviceInfoTypeID)

	// A reset chain answers chain_disabled.
	contents := sync_pb.ClientToServerMessage_CLEAR_SERVER_DATA
	a.do(&sync_pb.ClientToServerMessage{MessageContents: &contents,
		ClearServerData: &sync_pb.ClearServerDataMessage{}})
	resp, body = a.removeDevice("guid-b", "guid-a")
	assert.Equal(t, http.StatusForbidden, resp.StatusCode)
	assert.JSONEq(t, `{"error":"chain_disabled"}`, body)
}

// With company sign-in on, an unbound chain gets login_required.
func TestRemoveDeviceNeedsSignIn(t *testing.T) {
	base, _ := oidcServer(t)
	a := newDevice(t, base, randomSeed(t), "guid-a")
	resp, body := a.removeDevice("guid-b", "guid-a")
	assert.Equal(t, http.StatusUnauthorized, resp.StatusCode)
	assert.JSONEq(t, `{"error":"login_required"}`, body)
	assert.Equal(t, "required", resp.Header.Get("X-Compass-Login"))
}

func TestDevicesAdmin(t *testing.T) {
	base, path, _ := devicesServer(t)
	t.Setenv("COMPASS_SYNC_DB", path)
	seed := randomSeed(t)
	a := newDevice(t, base, seed, "guid-a")
	b := newDevice(t, base, seed, "guid-b")
	a.joinChain("Work Mac")
	b.joinChain("Phone")
	chain := strings.ToLower(a.chainID())

	var out, errOut bytes.Buffer
	assert.Equal(t, 2, server.Devices(nil, &out, &errOut))
	assert.Equal(t, 2, server.Devices([]string{"list"}, &out, &errOut))
	assert.Equal(t, 2, server.Devices([]string{"remove", chain}, &out, &errOut))
	require.Equal(t, 0, server.Devices([]string{"list", a.chainID()}, &out, &errOut), errOut.String())
	assert.Regexp(t, `CACHE GUID\s+NAME\s+OS\s+LAST UPDATED\s+STATUS`, out.String())
	assert.Regexp(t, `guid-a\s+Work Mac\s+mac\s+\d{4}-\d\d-\d\d \d\d:\d\dZ\s+active`, out.String())
	assert.Regexp(t, `guid-b\s+Phone\s+mac\s+\S+ \S+\s+active`, out.String())

	out.Reset()
	require.Equal(t, 0, server.Devices([]string{"remove", chain, "guid-b"}, &out, &errOut), errOut.String())
	assert.Contains(t, out.String(), "removed guid-b from chain "+chain+": its entry is deleted")
	out.Reset()
	require.Equal(t, 0, server.Devices([]string{"list", chain}, &out, &errOut))
	assert.Regexp(t, `guid-a\s+Work Mac\s+mac\s+\S+ \S+\s+active`, out.String())
	assert.Regexp(t, `guid-b\s+-\s+-\s+-\s+removed \d{4}-`, out.String())

	// The running server refuses it at once, and A gets the tombstone.
	assertRemoved(t, b)
	live, deleted := liveDevices(a.getUpdates(sync_pb.SyncEnums_GU_TRIGGER, deviceInfoTypeID))
	assert.Equal(t, []string{"guid-a"}, live) // Its own, committed after its last GetUpdates.
	assert.Equal(t, []string{deviceInfoTagHash("guid-b")}, deleted)

	assert.Equal(t, 1, server.Devices([]string{"remove", strings.Repeat("ab", 32), "g"}, &out, &errOut))
	assert.Contains(t, errOut.String(), "no chain")
}

// The JSON answers decode as clients expect.
func TestDeviceRemovedBody(t *testing.T) {
	base, _, _ := devicesServer(t)
	seed := randomSeed(t)
	a := newDevice(t, base, seed, "guid-a")
	a.joinChain("Mac")
	resp, _ := a.removeDevice("guid-z", "guid-a")
	require.Equal(t, http.StatusNoContent, resp.StatusCode)
	z := newDevice(t, base, seed, "guid-z")
	resp, body := z.call(http.MethodGet, "/v2/compass/events?client_id=guid-z", nil)
	var got map[string]string
	require.NoError(t, json.Unmarshal([]byte(body), &got))
	assert.Equal(t, "device_removed", got["error"])
	assert.Equal(t, "application/json", resp.Header.Get("Content-Type"))
}
