package server_test

// Rooms end to end (sync.md, "Rooms"): chains of different users share a
// room over HTTP with real tokens: roles, invites, the changes feed and its
// long poll, conflicts, key rotation, removal, the private server's gate,
// the directory on a company sign-in server, quotas and the admin
// commands.

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/server"
)

// roomsServer is a server on its own database with |maxBytes| per chain
// (0: the default); returns its URL and the database's path.
func roomsServer(t *testing.T, maxBytes int64) (string, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "rooms.db")
	cfg, err := server.LoadConfig()
	require.NoError(t, err)
	if maxBytes > 0 {
		cfg.MaxBytesPerChain = maxBytes
	}
	db, err := datastore.NewSQLite(path, datastore.SQLiteOptions{MaxBytesPerChain: cfg.MaxBytesPerChain})
	require.NoError(t, err)
	t.Cleanup(func() { db.Close() })
	ctx, logger := server.SetupLogger(context.Background())
	ctx, r, hub, err := server.SetupRouter(ctx, logger, cfg, db)
	require.NoError(t, err)
	srv := httptest.NewUnstartedServer(r)
	srv.Config.BaseContext = func(_ net.Listener) context.Context { return ctx }
	srv.Start()
	t.Cleanup(func() { hub.Close(); srv.Close() })
	return srv.URL, path
}

func b64(b []byte) string { return base64.StdEncoding.EncodeToString(b) }

// unknownRoom is a well-formed room ID no room has.
const unknownRoom = "AAAAAAAAAAAAAAAAAAAAAA"

// env is an 80-byte stand-in for an HPKE envelope, from |label|.
func env(label string) []byte {
	out := make([]byte, 0, 96)
	for i := 0; len(out) < 80; i++ {
		h := sha256.Sum256([]byte(label + string(rune('0'+i))))
		out = append(out, h[:]...)
	}
	return out[:80]
}

// ct is a stand-in for a record: nonce (12) || |text| || tag (16).
func ct(text string) []byte {
	out := append(make([]byte, 12), text...)
	return append(out, make([]byte, 16)...)
}

// secretFor is the 32-byte invite secret of |name|.
func secretFor(name string) []byte {
	h := sha256.Sum256([]byte("secret-" + name))
	return h[:]
}

// hashHex is the lower-case hex SHA-256 the server keeps of a secret.
func hashHex(secret []byte) string {
	h := sha256.Sum256(secret)
	return hex.EncodeToString(h[:])
}

// identity is the device's chain's identity key (any 32 bytes will do: the
// server never uses it).
func (d *device) identity() []byte {
	h := sha256.Sum256([]byte("identity " + d.chainID()))
	return h[:]
}

func (d *device) id() string { return strings.ToLower(d.chainID()) }

// api calls a JSON route; |body| is marshalled unless it's nil or a string
// (sent as is). Returns the status and the decoded answer (nil if none).
func (d *device) api(method, path string, body any) (int, map[string]any) {
	d.t.Helper()
	var raw []byte
	switch b := body.(type) {
	case nil:
	case string:
		raw = []byte(b)
	default:
		var err error
		raw, err = json.Marshal(body)
		require.NoError(d.t, err)
	}
	resp, text := d.call(method, path, raw)
	if text == "" {
		return resp.StatusCode, nil
	}
	var out map[string]any
	require.NoError(d.t, json.Unmarshal([]byte(text), &out), text)
	return resp.StatusCode, out
}

// mustAPI is api expecting |status|.
func (d *device) mustAPI(status int, method, path string, body any) map[string]any {
	d.t.Helper()
	code, out := d.api(method, path, body)
	require.Equal(d.t, status, code, "%s %s: %v", method, path, out)
	return out
}

func (d *device) createRoom(kind string) string {
	d.t.Helper()
	out := d.mustAPI(http.StatusCreated, "POST", "/v2/compass/rooms", map[string]any{
		"kind": kind, "identity_pub": b64(d.identity()), "envelope": b64(env("sealed-1-" + d.id()))})
	return out["id"].(string)
}

// makeInvite creates a link invite and returns its ID; the secret is
// "secret-" + |name| (16 bytes or more).
func (d *device) makeInvite(room, role, name string, uses int) string {
	d.t.Helper()
	out := d.mustAPI(http.StatusCreated, "POST", "/v2/compass/rooms/"+room+"/invites", map[string]any{
		"role": role, "secret_hash": hashHex(secretFor(name)), "expires": 0, "uses": uses})
	return out["id"].(string)
}

func (d *device) joinRoom(room, invite, name string) (int, map[string]any) {
	d.t.Helper()
	return d.api("POST", "/v2/compass/rooms/"+room+"/join", map[string]any{
		"invite_id": invite, "secret": base64.RawURLEncoding.EncodeToString(secretFor(name)),
		"identity_pub": b64(d.identity())})
}

func (d *device) write(room string, epoch int, records ...map[string]any) (int, map[string]any) {
	d.t.Helper()
	for _, r := range records {
		if _, ok := r["epoch"]; !ok {
			r["epoch"] = epoch
		}
	}
	return d.api("POST", "/v2/compass/rooms/"+room+"/records?client_id="+d.cacheGUID,
		map[string]any{"records": records})
}

func rec(id, text string) map[string]any {
	return map[string]any{"id": id, "ciphertext": b64(ct(text))}
}

// sharedRoom: A owns a folder room, B joined as editor, C as viewer.
func sharedRoom(t *testing.T, base string) (a, b, c *device, room string) {
	a = newDevice(t, base, randomSeed(t), "guid-a")
	b = newDevice(t, base, randomSeed(t), "guid-b")
	c = newDevice(t, base, randomSeed(t), "guid-c")
	room = a.createRoom("folder")
	code, out := b.joinRoom(room, a.makeInvite(room, "editor", "b", 0), "b")
	require.Equal(t, http.StatusOK, code, out)
	code, out = c.joinRoom(room, a.makeInvite(room, "viewer", "c", 0), "c")
	require.Equal(t, http.StatusOK, code, out)
	return a, b, c, room
}

func TestEndToEndRooms(t *testing.T) {
	base, _ := roomsServer(t, 0)
	a, b, c, room := sharedRoom(t, base)
	d := newDevice(t, base, randomSeed(t), "guid-d")

	view := a.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, "folder", view["kind"])
	assert.Equal(t, a.id(), view["owner"])
	assert.Equal(t, "owner", view["role"])
	assert.EqualValues(t, 1, view["epoch"])
	assert.EqualValues(t, 0, view["seq"])
	assert.Len(t, view["members"], 3)
	assert.Len(t, view["invites"], 2)
	envs := view["envelopes"].([]any)
	require.Len(t, envs, 1)
	assert.Equal(t, b64(env("sealed-1-"+a.id())), envs[0].(map[string]any)["sealed"])
	for _, m := range view["members"].([]any) {
		m := m.(map[string]any)
		if m["client_id"] == b.id() {
			assert.Equal(t, "editor", m["role"])
			assert.Equal(t, b64(b.identity()), m["identity_pub"])
			assert.Equal(t, false, m["pending"])
		}
	}
	view = c.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, "viewer", view["role"])
	assert.Nil(t, view["invites"], "only the owner sees invites")
	list := b.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms", nil)
	assert.Equal(t, []any{map[string]any{"id": room, "kind": "folder", "owner": a.id(), "role": "editor",
		"pending": false, "epoch": 1.0, "seq": 0.0}}, list["rooms"])

	// Change streams: B and C hear A's commit as a "room" event; A's own
	// device doesn't.
	sa, _ := a.subscribe(a.token(time.Now()))
	sb, _ := b.subscribe(b.token(time.Now()))
	sc, _ := c.subscribe(c.token(time.Now()))

	code, out := a.write(room, 1, rec("node:1", "hello"), rec("node:2", "world"))
	require.Equal(t, http.StatusOK, code, out)
	assert.EqualValues(t, 2, out["seq"])
	assert.Equal(t, []any{map[string]any{"id": "node:1", "seq": 1.0}, map[string]any{"id": "node:2", "seq": 2.0}},
		out["records"])
	event := `data: {"room_id":"` + room + `","seq":2,"changes":["records"]}`
	sb.expect("event: room", event, "")
	sc.expect("event: room", event, "")
	sa.quiet()

	// Everyone joined reads; the viewer can't write; strangers see nothing.
	out = c.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room+"/changes?since=0", nil)
	assert.EqualValues(t, 2, out["seq"])
	assert.Equal(t, false, out["more"])
	recs := out["records"].([]any)
	require.Len(t, recs, 2)
	assert.Equal(t, map[string]any{"id": "node:1", "seq": 1.0, "epoch": 1.0, "deleted": false, "author": a.id(),
		"ciphertext": b64(ct("hello"))}, recs[0])
	code, out = c.write(room, 1, rec("node:3", "nope"))
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, "forbidden", out["error"])
	code, out = b.write(room, 1, rec("node:3", "edit"))
	assert.Equal(t, http.StatusOK, code, out)
	// The editor's commit reaches the owner (another device), the viewer
	// and the editor's own other devices, not the committing device.
	ev3 := `data: {"room_id":"` + room + `","seq":3,"changes":["records"]}`
	sa.expect("event: room", ev3, "")
	sc.expect("event: room", ev3, "")
	sb.quiet()
	out = b.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room+"/changes?since=2&limit=10", nil)
	assert.Len(t, out["records"], 1)

	for _, path := range []string{"", "/changes"} {
		code, out = d.api("GET", "/v2/compass/rooms/"+room+path, nil)
		assert.Equal(t, http.StatusNotFound, code, path)
		assert.Equal(t, "not_found", out["error"])
	}
	assert.Regexp(t, `^[A-Za-z0-9_-]{22}$`, room, "16 bytes, base64url without padding")
	for _, bad := range []string{"not-a-room", strings.ToLower(room) + "x", "AAAAAAAAAAAAAAAAAAAAAB"} {
		code, _ = a.api("GET", "/v2/compass/rooms/"+bad, nil)
		assert.Equal(t, http.StatusNotFound, code, bad)
	}
	// Sizes: envelopes are 80 bytes, records 28 or more, keys 32.
	code, _ = a.api("POST", "/v2/compass/rooms", map[string]any{"kind": "folder",
		"identity_pub": b64(a.identity()), "envelope": b64(env("x")[:79])})
	assert.Equal(t, http.StatusBadRequest, code)
	code, _ = b.write(room, 1, map[string]any{"id": "short", "ciphertext": b64(make([]byte, 27))})
	assert.Equal(t, http.StatusBadRequest, code)
	code, _ = d.api("GET", "/v2/compass/rooms/not-a-room", nil)
	assert.Equal(t, http.StatusNotFound, code)
	code, _ = d.api("GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, http.StatusNotFound, code)
	code, out = d.write(room, 1, rec("x", "y"))
	assert.Equal(t, http.StatusNotFound, code, out)
	list = d.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms", nil)
	assert.Equal(t, []any{}, list["rooms"])

	// No token.
	req, err := http.NewRequest("GET", base+"/v2/compass/rooms/"+room, nil)
	require.NoError(t, err)
	resp, err := http.DefaultClient.Do(req)
	require.NoError(t, err)
	assert.Equal(t, http.StatusUnauthorized, resp.StatusCode)
	assert.JSONEq(t, `{"error":"invalid sync token"}`, readAll(t, resp))

	// Members: a removal is announced to all, the removed one included.
	a.mustAPI(http.StatusNoContent, "DELETE", "/v2/compass/rooms/"+room+"/members/"+c.id(), nil)
	sc.expect("event: room", `data: {"room_id":"`+room+`","seq":3,"changes":["members"]}`, "")
	sb.expect("event: room", `data: {"room_id":"`+room+`","seq":3,"changes":["members"]}`, "")

	// Deleting the room.
	code, out = b.api("DELETE", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, http.StatusForbidden, code, out)
	a.mustAPI(http.StatusNoContent, "DELETE", "/v2/compass/rooms/"+room, nil)
	sb.expect("event: room", `data: {"room_id":"`+room+`","seq":3,"changes":["deleted"]}`, "")
	code, _ = b.api("GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, http.StatusNotFound, code)
}

func TestRoomsOwnerOnlyRoutes(t *testing.T) {
	base, _ := roomsServer(t, 0)
	a, b, c, room := sharedRoom(t, base)
	inv := a.makeInvite(room, "viewer", "x", 0)
	envelope := []map[string]any{{"client_id": b.id(), "epoch": 1, "sealed": b64(env("e"))}}
	for _, who := range []*device{b, c} {
		for _, r := range []struct {
			method, path string
			body         any
		}{
			{"POST", "/invites", map[string]any{"role": "viewer", "secret_hash": hashHex(secretFor("s"))}},
			{"DELETE", "/invites/" + inv, nil},
			{"PATCH", "/members/" + c.id(), map[string]any{"role": "editor"}},
			{"DELETE", "/members/" + a.id(), nil},
			{"POST", "/owner", map[string]any{"client_id": who.id()}},
			{"POST", "/epochs", map[string]any{"epoch": 2, "envelopes": envelope}},
			{"POST", "/envelopes", map[string]any{"envelopes": envelope}},
			{"DELETE", "", nil},
		} {
			code, out := who.api(r.method, "/v2/compass/rooms/"+room+r.path, r.body)
			assert.Equal(t, http.StatusForbidden, code, "%s %s by %s: %v", r.method, r.path, who.cacheGUID, out)
			assert.Equal(t, "forbidden", out["error"])
		}
	}
	// A member sets its own profile, not another's.
	c.mustAPI(http.StatusNoContent, "PATCH", "/v2/compass/rooms/"+room+"/members/"+c.id(),
		map[string]any{"profile": b64([]byte("encrypted name"))})
	code, out := c.api("PATCH", "/v2/compass/rooms/"+room+"/members/"+b.id(), map[string]any{"profile": b64([]byte("x"))})
	assert.Equal(t, http.StatusForbidden, code, out)
	code, _ = c.api("PATCH", "/v2/compass/rooms/"+room+"/members/"+c.id(), map[string]any{})
	assert.Equal(t, http.StatusBadRequest, code)
	code, _ = a.api("PATCH", "/v2/compass/rooms/"+room+"/members/"+c.id(), map[string]any{"role": "owner"})
	assert.Equal(t, http.StatusBadRequest, code)
	// The owner promotes the viewer, who then writes.
	a.mustAPI(http.StatusNoContent, "PATCH", "/v2/compass/rooms/"+room+"/members/"+c.id(), map[string]any{"role": "editor"})
	code, _ = c.write(room, 1, rec("n", "x"))
	assert.Equal(t, http.StatusOK, code)
	view := a.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	for _, m := range view["members"].([]any) {
		if m := m.(map[string]any); m["client_id"] == c.id() {
			assert.Equal(t, b64([]byte("encrypted name")), m["profile"])
			assert.Equal(t, "editor", m["role"])
		}
	}
}

func TestRoomsInvitesHTTP(t *testing.T) {
	base, _ := roomsServer(t, 0)
	a := newDevice(t, base, randomSeed(t), "guid-a")
	room := a.createRoom("easel")
	once := a.makeInvite(room, "viewer", "once", 1)
	revoked := a.makeInvite(room, "editor", "revoked", 0)
	a.mustAPI(http.StatusNoContent, "DELETE", "/v2/compass/rooms/"+room+"/invites/"+revoked, nil)
	code, out := a.api("DELETE", "/v2/compass/rooms/"+room+"/invites/ffffffffffffffffffffffffffffffff", nil)
	assert.Equal(t, http.StatusNotFound, code)
	assert.Equal(t, "invite_not_found", out["error"])

	// Expiring in 1.5 s.
	h := hashHex(secretFor("soon"))
	out = a.mustAPI(http.StatusCreated, "POST", "/v2/compass/rooms/"+room+"/invites", map[string]any{
		"role": "editor", "secret_hash": h, "expires": time.Now().Add(1500 * time.Millisecond).UnixMilli()})
	soon := out["id"].(string)
	assert.Regexp(t, `^[A-Za-z0-9_-]{22}$`, soon, "16 bytes, base64url")
	assert.Nil(t, out["uses_left"])
	assert.Equal(t, false, out["revoked"])

	b := newDevice(t, base, randomSeed(t), "guid-b")
	c := newDevice(t, base, randomSeed(t), "guid-c")
	for _, tc := range []struct {
		invite, name, want string
	}{
		{once, "wrong", "invite_invalid"},
		{"ffffffffffffffffffffffffffffffff", "once", "invite_invalid"},
		{"bad", "once", "invite_invalid"},
		{revoked, "revoked", "invite_revoked"},
	} {
		code, out := b.joinRoom(room, tc.invite, tc.name)
		assert.Equal(t, http.StatusForbidden, code, tc)
		assert.Equal(t, tc.want, out["error"], tc)
	}
	// The secret is 32 bytes, unpadded base64url (the link's "x").
	for _, secret := range []string{base64.RawURLEncoding.EncodeToString(secretFor("once")[:16]),
		base64.URLEncoding.EncodeToString(secretFor("once")), b64(secretFor("once"))} {
		code, out := b.api("POST", "/v2/compass/rooms/"+room+"/join", map[string]any{
			"invite_id": once, "secret": secret, "identity_pub": b64(b.identity())})
		assert.Equal(t, http.StatusForbidden, code, secret)
		assert.Equal(t, "invite_invalid", out["error"], secret)
	}
	// An unknown room looks like a bad invite.
	code, out = b.joinRoom(unknownRoom, once, "once")
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, "invite_invalid", out["error"])

	code, out = b.joinRoom(room, once, "once")
	require.Equal(t, http.StatusOK, code, out)
	assert.Equal(t, "viewer", out["role"])
	code, out = c.joinRoom(room, once, "once")
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, "invite_used_up", out["error"])

	time.Sleep(1600 * time.Millisecond)
	code, out = c.joinRoom(room, soon, "soon")
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, "invite_expired", out["error"])

	// Bad requests.
	for _, body := range []any{
		map[string]any{"role": "owner", "secret_hash": h},
		map[string]any{"role": "viewer", "secret_hash": b64(secretFor("x"))},
		map[string]any{"role": "viewer", "secret_hash": strings.ToUpper(h)},
		map[string]any{"role": "viewer", "secret_hash": h, "expires": 1},
		map[string]any{"role": "viewer", "secret_hash": h, "uses": -1},
		"{",
		`{"role":"viewer"} {}`,
	} {
		code, _ := a.api("POST", "/v2/compass/rooms/"+room+"/invites", body)
		assert.Equal(t, http.StatusBadRequest, code, body)
	}
	code, _ = c.api("POST", "/v2/compass/rooms/"+room+"/join", map[string]any{"identity_pub": b64([]byte("short"))})
	assert.Equal(t, http.StatusBadRequest, code)

	// The owner's view.
	view := a.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	byID := map[string]map[string]any{}
	for _, inv := range view["invites"].([]any) {
		byID[inv.(map[string]any)["id"].(string)] = inv.(map[string]any)
	}
	assert.EqualValues(t, 0, byID[once]["uses_left"])
	assert.Equal(t, true, byID[revoked]["revoked"])
	assert.NotContains(t, byID[once], "secret_hash")
}

func TestRoomsLongPoll(t *testing.T) {
	base, _ := roomsServer(t, 0)
	a, b, c, room := sharedRoom(t, base)
	_ = a

	// Nothing new: answers empty after the wait.
	start := time.Now()
	out := c.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room+"/changes?since=0&wait=1", nil)
	assert.GreaterOrEqual(t, time.Since(start), 900*time.Millisecond)
	assert.Equal(t, []any{}, out["records"])
	assert.EqualValues(t, 0, out["seq"])

	// A commit wakes it at once.
	done := make(chan map[string]any)
	go func() {
		_, out := c.api("GET", "/v2/compass/rooms/"+room+"/changes?since=0&wait=30", nil)
		done <- out
	}()
	time.Sleep(200 * time.Millisecond)
	start = time.Now()
	code, _ := b.write(room, 1, rec("r", "x"))
	require.Equal(t, http.StatusOK, code)
	select {
	case out = <-done:
		assert.Len(t, out["records"], 1)
		assert.Less(t, time.Since(start), 5*time.Second)
	case <-time.After(10 * time.Second):
		t.Fatal("the long poll wasn't woken")
	}

	// A removal wakes it too, and it then says so.
	go func() {
		code, out := c.api("GET", "/v2/compass/rooms/"+room+"/changes?since=1&wait=30", nil)
		out["status"] = code
		done <- out
	}()
	time.Sleep(200 * time.Millisecond)
	a.mustAPI(http.StatusNoContent, "DELETE", "/v2/compass/rooms/"+room+"/members/"+c.id(), nil)
	select {
	case out = <-done:
		assert.EqualValues(t, http.StatusForbidden, out["status"])
		assert.Equal(t, "removed", out["error"])
	case <-time.After(10 * time.Second):
		t.Fatal("the long poll wasn't woken")
	}

	for _, q := range []string{"since=-1", "since=x", "wait=31", "limit=0", "limit=1001"} {
		code, _ := b.api("GET", "/v2/compass/rooms/"+room+"/changes?"+q, nil)
		assert.Equal(t, http.StatusBadRequest, code, q)
	}
}

func TestRoomsConflictsHTTP(t *testing.T) {
	base, _ := roomsServer(t, 0)
	a, b, _, room := sharedRoom(t, base)
	code, _ := a.write(room, 1, rec("x", "v1"))
	require.Equal(t, http.StatusOK, code)
	code, _ = b.write(room, 1, map[string]any{"id": "x", "ciphertext": b64(ct("v2")), "base_seq": 1})
	require.Equal(t, http.StatusOK, code)

	code, out := a.write(room, 1, map[string]any{"id": "x", "ciphertext": b64(ct("v3")), "base_seq": 1},
		map[string]any{"id": "new", "ciphertext": b64(ct("n")), "base_seq": 0})
	assert.Equal(t, http.StatusConflict, code)
	assert.Equal(t, "conflict", out["error"])
	assert.Equal(t, []any{map[string]any{"id": "x", "seq": 2.0, "epoch": 1.0, "deleted": false, "author": b.id(),
		"ciphertext": b64(ct("v2"))}}, out["current"])
	out = a.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room+"/changes?since=2", nil)
	assert.Equal(t, []any{}, out["records"], "nothing of the refused batch")

	// A delete, then a create-only write of a deleted record conflicts.
	code, _ = a.write(room, 1, map[string]any{"id": "x", "deleted": true})
	require.Equal(t, http.StatusOK, code)
	code, out = a.write(room, 1, map[string]any{"id": "x", "ciphertext": b64(ct("again")), "base_seq": 0})
	assert.Equal(t, http.StatusConflict, code)
	assert.Equal(t, []any{map[string]any{"id": "x", "seq": 3.0, "epoch": 1.0, "deleted": true, "author": a.id()}},
		out["current"])

	for _, records := range [][]map[string]any{
		{},
		{{"id": "bad id", "ciphertext": "eA=="}},
		{{"id": "a", "ciphertext": "eA=="}, {"id": "a", "ciphertext": "eA=="}},
		{{"id": "a"}},
		{{"id": "a", "ciphertext": "eA==", "epoch": 0}},
		{{"id": "a", "ciphertext": "eA==", "base_seq": -1}},
		{{"id": "a", "ciphertext": "not base64!"}},
	} {
		code, out := a.write(room, 1, records...)
		assert.Equal(t, http.StatusBadRequest, code, "%v: %v", records, out)
	}
	code, out = a.write(room, 2, rec("a", "x"))
	assert.Equal(t, http.StatusConflict, code)
	assert.Equal(t, "stale_epoch", out["error"])
}

func TestRoomsRotationAndRemoval(t *testing.T) {
	base, _ := roomsServer(t, 0)
	a, b, c, room := sharedRoom(t, base)

	// Leaving and the owner.
	code, out := a.api("DELETE", "/v2/compass/rooms/"+room+"/members/"+a.id(), nil)
	assert.Equal(t, http.StatusConflict, code)
	assert.Equal(t, "owner_must_transfer", out["error"])
	code, out = a.api("DELETE", "/v2/compass/rooms/"+room+"/members/"+strings.Repeat("0", 64), nil)
	assert.Equal(t, http.StatusNotFound, code)
	assert.Equal(t, "member_not_found", out["error"])

	// Remove C, rotate.
	a.mustAPI(http.StatusNoContent, "DELETE", "/v2/compass/rooms/"+room+"/members/"+c.id(), nil)
	code, out = c.api("GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, "removed", out["error"])
	code, out = c.api("GET", "/v2/compass/rooms/"+room+"/changes", nil)
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, "removed", out["error"])
	// The link it joined with doesn't bring it back.
	code, out = c.joinRoom(room, a.makeInvite(room, "viewer", "c", 0), "nope")
	assert.Equal(t, http.StatusForbidden, code, out)

	envFor := func(d *device) map[string]any {
		return map[string]any{"client_id": d.id(), "sealed": b64(env("sealed-2-" + d.id()))}
	}
	code, out = a.api("POST", "/v2/compass/rooms/"+room+"/epochs", map[string]any{"epoch": 2,
		"envelopes": []any{envFor(a)}})
	assert.Equal(t, http.StatusConflict, code)
	assert.Equal(t, map[string]any{"error": "envelopes_mismatch", "missing": []any{b.id()}, "unknown": []any{}}, out)
	code, out = a.api("POST", "/v2/compass/rooms/"+room+"/epochs", map[string]any{"epoch": 2,
		"envelopes": []any{envFor(a), envFor(b), envFor(c)}})
	assert.Equal(t, http.StatusConflict, code)
	assert.Equal(t, []any{c.id()}, out["unknown"])
	code, out = a.api("POST", "/v2/compass/rooms/"+room+"/epochs", map[string]any{"epoch": 3,
		"envelopes": []any{envFor(a), envFor(b)}})
	assert.Equal(t, http.StatusConflict, code)
	assert.Equal(t, "stale_epoch", out["error"])
	sb, _ := b.subscribe(b.token(time.Now()))
	a.mustAPI(http.StatusNoContent, "POST", "/v2/compass/rooms/"+room+"/epochs", map[string]any{"epoch": 2,
		"envelopes": []any{envFor(a), envFor(b)}})
	sb.expect("event: room", `data: {"room_id":"`+room+`","seq":0,"changes":["keys"]}`, "")

	view := b.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	assert.EqualValues(t, 2, view["epoch"])
	assert.Equal(t, []any{map[string]any{"epoch": 2.0, "sealed": b64(env("sealed-2-" + b.id()))}}, view["envelopes"])
	code, out = b.write(room, 1, rec("n", "old key"))
	assert.Equal(t, http.StatusConflict, code)
	assert.Equal(t, "stale_epoch", out["error"])
	code, _ = b.write(room, 2, rec("n", "new key"))
	assert.Equal(t, http.StatusOK, code)
	// Past epochs' keys for a member (B joined by a link of epoch 1).
	a.mustAPI(http.StatusNoContent, "POST", "/v2/compass/rooms/"+room+"/envelopes", map[string]any{
		"envelopes": []any{map[string]any{"client_id": b.id(), "epoch": 1, "sealed": b64(env("e1"))}}})
	view = b.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	assert.Len(t, view["envelopes"], 2)

	// Transfer, then the old owner leaves.
	code, out = a.api("POST", "/v2/compass/rooms/"+room+"/owner", map[string]any{"client_id": c.id()})
	assert.Equal(t, http.StatusNotFound, code, out)
	a.mustAPI(http.StatusNoContent, "POST", "/v2/compass/rooms/"+room+"/owner", map[string]any{"client_id": b.id()})
	a.mustAPI(http.StatusNoContent, "DELETE", "/v2/compass/rooms/"+room+"/members/"+a.id(), nil)
	view = b.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, b.id(), view["owner"])
	assert.Len(t, view["members"], 1)
	assert.NotNil(t, view["invites"])
}

func TestRoomsLimitsHTTP(t *testing.T) {
	base, _ := roomsServer(t, 3<<20)
	a, b, _, room := sharedRoom(t, base)
	code, out := b.write(room, 1, map[string]any{"id": "big", "ciphertext": b64(make([]byte, datastore.MaxRoomRecordBytes+1))})
	assert.Equal(t, http.StatusRequestEntityTooLarge, code)
	assert.Equal(t, "record_too_large", out["error"])
	for i := range 2 {
		code, out = b.write(room, 1, map[string]any{"id": "r" + string(rune('0'+i)),
			"ciphertext": b64(make([]byte, datastore.MaxRoomRecordBytes))})
		require.Equal(t, http.StatusOK, code, out)
	}
	_, quota := a.api("GET", "/v2/compass/quota", nil)
	assert.InDelta(t, float64(2<<20), quota["bytes"], 100, "the room counts against the owner's chain")
	code, out = b.write(room, 1, map[string]any{"id": "r9", "ciphertext": b64(make([]byte, datastore.MaxRoomRecordBytes))})
	assert.Equal(t, http.StatusInsufficientStorage, code)
	assert.Equal(t, "quota_exceeded", out["error"])
	_, quota = b.api("GET", "/v2/compass/quota", nil)
	assert.EqualValues(t, 0, quota["bytes"], "not the author's")
}

func TestRoomsPrivateServerGate(t *testing.T) {
	base, _ := roomsServer(t, 0)
	a := newDevice(t, base, randomSeed(t), "guid-a")
	b := newDevice(t, base, randomSeed(t), "guid-b")
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", a.id())
	room := a.createRoom("space")
	inv := a.makeInvite(room, "editor", "b", 0)

	// B isn't listed: no personal sync, no rooms of its own, nothing of a
	// room before it holds a valid invite.
	code, _ := b.post(getUpdatesMsg(), b.token(time.Now()))
	assert.Equal(t, http.StatusUnauthorized, code)
	resp, _ := b.call("GET", "/v2/compass/quota", nil)
	assert.Equal(t, http.StatusUnauthorized, resp.StatusCode)
	notAllowed := map[string]any{"error": "chain_not_allowed"}
	for _, r := range []struct{ method, path string }{
		{"POST", "/v2/compass/rooms"},
		{"GET", "/v2/compass/rooms"},
		{"GET", "/v2/compass/rooms/" + room},
		{"GET", "/v2/compass/rooms/" + room + "/changes"},
		{"PUT", "/v2/compass/identity"},
		{"GET", "/v2/compass/directory?email=a@example.com"},
	} {
		code, out := b.api(r.method, r.path, "{}")
		assert.Equal(t, http.StatusForbidden, code, r.path)
		assert.Equal(t, notAllowed, out, r.path)
	}
	code, out := b.joinRoom(room, inv, "wrong")
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, notAllowed, out)

	// With the invite: the room's routes and its identity key, nothing else.
	code, out = b.joinRoom(room, inv, "b")
	require.Equal(t, http.StatusOK, code, out)
	b.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	b.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms", nil)
	b.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room+"/changes", nil)
	code, _ = b.write(room, 1, rec("x", "y"))
	assert.Equal(t, http.StatusOK, code)
	b.mustAPI(http.StatusNoContent, "PUT", "/v2/compass/identity", map[string]any{"identity_pub": b64(b.identity())})
	out = b.mustAPI(http.StatusOK, "GET", "/v2/compass/identity", nil)
	assert.Equal(t, b64(b.identity()), out["identity_pub"])
	code, out = b.api("POST", "/v2/compass/rooms", map[string]any{"kind": "folder",
		"identity_pub": b64(b.identity()), "envelope": "eA=="})
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, notAllowed, out)
	code, _ = b.post(getUpdatesMsg(), b.token(time.Now()))
	assert.Equal(t, http.StatusUnauthorized, code, "personal sync still refused")
	other := a.createRoom("folder")
	code, out = b.api("GET", "/v2/compass/rooms/"+other, nil)
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, notAllowed, out, "only the rooms it's in")

	// Removed: it's told so; then it has no room left.
	a.mustAPI(http.StatusNoContent, "DELETE", "/v2/compass/rooms/"+room+"/members/"+b.id(), nil)
	code, out = b.api("GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, "removed", out["error"])
	code, out = b.api("GET", "/v2/compass/rooms", nil)
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, notAllowed, out)
}

func TestRoomsDirectoryOffWithoutSignIn(t *testing.T) {
	base, _ := roomsServer(t, 0)
	a, b, _, room := sharedRoom(t, base)
	code, out := a.api("GET", "/v2/compass/directory?email=b@example.com", nil)
	assert.Equal(t, http.StatusNotFound, code)
	assert.Equal(t, "directory_disabled", out["error"])
	code, out = a.api("POST", "/v2/compass/rooms/"+room+"/members", map[string]any{"members": []any{
		map[string]any{"client_id": b.id(), "role": "viewer", "identity_pub": b64(b.identity()),
			"envelopes": []any{map[string]any{"epoch": 1, "sealed": "eA=="}}}}})
	assert.Equal(t, http.StatusNotFound, code)
	assert.Equal(t, "directory_disabled", out["error"])
}

// signIn binds |d|'s chain to |email| on a fake-IdP server.
func signIn(t *testing.T, base string, d *device, email string) {
	t.Helper()
	resp, body := d.call("POST", "/v2/compass/login", []byte(`{"device_name":"Test"}`))
	require.Equal(t, http.StatusOK, resp.StatusCode, body)
	var created map[string]any
	require.NoError(t, json.Unmarshal([]byte(body), &created))
	signInWithBrowser(t, base, created["url"].(string), email)
}

func TestRoomsWithCompanySignIn(t *testing.T) {
	base, dbPath := oidcServer(t)
	a := newDevice(t, base, randomSeed(t), "guid-a")
	b := newDevice(t, base, randomSeed(t), "guid-b")
	stranger := newDevice(t, base, randomSeed(t), "guid-s")

	// Rooms need sign-in too.
	for _, r := range []struct{ method, path string }{
		{"POST", "/v2/compass/rooms"},
		{"GET", "/v2/compass/rooms"},
		{"GET", "/v2/compass/rooms/0123456789abcdef0123456789abcdef"},
		{"POST", "/v2/compass/rooms/0123456789abcdef0123456789abcdef/join"},
		{"PUT", "/v2/compass/identity"},
		{"GET", "/v2/compass/directory?email=a@example.com"},
	} {
		resp, body := stranger.call(r.method, r.path, []byte(`{}`))
		assert.Equal(t, http.StatusUnauthorized, resp.StatusCode, r.path)
		assert.JSONEq(t, `{"error":"login_required"}`, body, r.path)
	}

	signIn(t, base, a, "alice@example.com")
	signIn(t, base, b, "bob@example.com")
	room := a.createRoom("space")

	// Bob's account isn't in the directory before it registers its key.
	out := a.mustAPI(http.StatusOK, "GET", "/v2/compass/directory?email=BOB@example.com", nil)
	assert.Equal(t, []any{}, out["accounts"])
	b.mustAPI(http.StatusNoContent, "PUT", "/v2/compass/identity", map[string]any{"identity_pub": b64(b.identity())})
	out = a.mustAPI(http.StatusOK, "GET", "/v2/compass/directory?email=BOB@example.com", nil)
	assert.Equal(t, []any{map[string]any{"client_id": b.id(), "identity_pub": b64(b.identity()), "name": "Alice",
		"email": "bob@example.com"}}, out["accounts"])
	code, _ := a.api("GET", "/v2/compass/directory", nil)
	assert.Equal(t, http.StatusBadRequest, code)

	// Email invite: B is pending, hears of it, sees the room, then joins.
	sb, _ := b.subscribe(b.token(time.Now()))
	member := func(d *device, key []byte) map[string]any {
		return map[string]any{"client_id": d.id(), "role": "editor", "identity_pub": b64(key),
			"envelopes": []any{map[string]any{"epoch": 1, "sealed": b64(env("for " + d.id()))}}}
	}
	code, out = a.api("POST", "/v2/compass/rooms/"+room+"/members", map[string]any{"members": []any{member(b, bytes.Repeat([]byte{7}, 32))}})
	assert.Equal(t, http.StatusConflict, code)
	assert.Equal(t, "identity_mismatch", out["error"])
	code, out = a.api("POST", "/v2/compass/rooms/"+room+"/members", map[string]any{"members": []any{member(stranger, stranger.identity())}})
	assert.Equal(t, http.StatusConflict, code)
	assert.Equal(t, map[string]any{"error": "not_signed_in", "client_id": stranger.id()}, out)
	view := a.mustAPI(http.StatusOK, "POST", "/v2/compass/rooms/"+room+"/members", map[string]any{"members": []any{member(b, b.identity())}})
	assert.Len(t, view["members"], 2)
	sb.expect("event: room", `data: {"room_id":"`+room+`","seq":0,"changes":["members"]}`, "")
	list := b.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms", nil)
	require.Len(t, list["rooms"], 1)
	assert.Equal(t, true, list["rooms"].([]any)[0].(map[string]any)["pending"])
	view = b.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, true, view["pending"])
	assert.Equal(t, []any{map[string]any{"epoch": 1.0, "sealed": b64(env("for " + b.id()))}}, view["envelopes"])
	code, out = b.api("GET", "/v2/compass/rooms/"+room+"/changes", nil)
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, "not_joined", out["error"])
	view = b.mustAPI(http.StatusOK, "POST", "/v2/compass/rooms/"+room+"/join", map[string]any{"identity_pub": b64(b.identity())})
	assert.Equal(t, false, view["pending"])
	for _, m := range view["members"].([]any) {
		m := m.(map[string]any)
		if m["client_id"] == a.id() {
			assert.Equal(t, "alice@example.com", m["email"], "from sign-in")
		}
	}
	code, _ = b.write(room, 1, rec("x", "y"))
	assert.Equal(t, http.StatusOK, code)

	// Revoking Bob with his data takes him out of the room.
	t.Setenv("COMPASS_SYNC_DB", dbPath)
	var stdout, stderr bytes.Buffer
	require.Equal(t, 0, server.Users([]string{"revoke", "bob@example.com", "--delete-data"}, &stdout, &stderr), stderr.String())
	view = a.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	assert.Len(t, view["members"], 1)
	// Alice's, with hers: the room goes.
	require.Equal(t, 0, server.Users([]string{"revoke", "alice@example.com", "--delete-data"}, &stdout, &stderr), stderr.String())
	stdout.Reset()
	require.Equal(t, 0, server.Rooms([]string{"list"}, &stdout, &stderr))
	assert.NotContains(t, stdout.String(), room)
}

func TestRoomsAdmin(t *testing.T) {
	base, path := roomsServer(t, 0)
	t.Setenv("COMPASS_SYNC_DB", path)
	a, b, c, room := sharedRoom(t, base)
	code, _ := b.write(room, 1, rec("x", "y"))
	require.Equal(t, http.StatusOK, code)
	a.mustAPI(http.StatusNoContent, "DELETE", "/v2/compass/rooms/"+room+"/members/"+c.id(), nil)

	var out, errOut bytes.Buffer
	assert.Equal(t, 2, server.Rooms(nil, &out, &errOut))
	assert.Equal(t, 2, server.Rooms([]string{"show"}, &out, &errOut))
	assert.Equal(t, 2, server.Rooms([]string{"nope", "x"}, &out, &errOut))
	require.Equal(t, 0, server.Rooms([]string{"list"}, &out, &errOut), errOut.String())
	assert.Regexp(t, `ROOM\s+KIND\s+OWNER\s+MEMBERS\s+EPOCH\s+SEQ\s+BYTES\s+CREATED`, out.String())
	assert.Regexp(t, room+`\s+folder\s+`+a.id()+`\s+2\s+1\s+1\s+30\s+\d{4}-`, out.String())

	out.Reset()
	require.Equal(t, 0, server.Rooms([]string{"show", room}, &out, &errOut), errOut.String())
	assert.Contains(t, out.String(), "room "+room+" (folder)")
	assert.Contains(t, out.String(), "epoch 1, seq 1, 1 records, 30 bytes")
	assert.Regexp(t, a.id()+`\s+owner\s+member`, out.String())
	assert.Regexp(t, b.id()+`\s+editor\s+member`, out.String())
	assert.Regexp(t, c.id()+`\s+viewer\s+removed \d{4}-`, out.String())
	assert.Regexp(t, `INVITE\s+ROLE\s+CREATED\s+EXPIRES\s+USES LEFT\s+STATUS`, out.String())
	assert.Regexp(t, `[A-Za-z0-9_-]{22}\s+editor\s+\S+ \S+\s+never\s+unlimited\s+active`, out.String())

	assert.Equal(t, 1, server.Rooms([]string{"show", unknownRoom}, &out, &errOut))
	out.Reset()
	require.Equal(t, 0, server.Rooms([]string{"delete", room}, &out, &errOut), errOut.String())
	assert.Contains(t, out.String(), "deleted room "+room+" (2 member(s))")
	assert.Equal(t, 1, server.Rooms([]string{"delete", room}, &out, &errOut))
	code, _ = b.api("GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, http.StatusNotFound, code)
}

// A reset: each member's old chain hands its membership to the account's
// new chain before the old one is cleared, owner included; on a private
// server the new chains aren't listed.
func TestRoomsSuccessorOnReset(t *testing.T) {
	base, path := roomsServer(t, 0)
	a, b, c, room := sharedRoom(t, base)
	code, _ := b.write(room, 1, rec("x", "before the reset"))
	require.Equal(t, http.StatusOK, code)
	t.Setenv("COMPASS_SYNC_ALLOWED_CHAINS", a.id()+","+b.id()+","+c.id())
	a2 := newDevice(t, base, randomSeed(t), "guid-a2")
	c2 := newDevice(t, base, randomSeed(t), "guid-c2")
	body := func(d *device) map[string]any {
		return map[string]any{"identity_pub": b64(d.identity()),
			"envelopes": []any{map[string]any{"epoch": 1, "sealed": b64(env("for " + d.id()))}}}
	}
	succeed := func(old, next *device, route string, extra ...string) (*http.Response, string) {
		raw, err := json.Marshal(body(next))
		require.NoError(t, err)
		hdr := append([]string{"X-Compass-Linked-Token", next.token(time.Now())}, extra...)
		return old.call("POST", "/v2/compass/rooms/"+room+route, raw, hdr...)
	}

	// Bad requests.
	resp, text := a.call("POST", "/v2/compass/rooms/"+room+"/successor", []byte(`{}`), "X-Compass-Linked-Token", "nope")
	assert.Equal(t, http.StatusBadRequest, resp.StatusCode)
	assert.JSONEq(t, `{"error":"invalid_linked_token"}`, text)
	resp, _ = succeed(a, a, "/successor")
	assert.Equal(t, http.StatusBadRequest, resp.StatusCode, "its own chain")
	resp, _ = succeed(a, b, "/successor")
	assert.Equal(t, http.StatusConflict, resp.StatusCode, "already a member")
	raw, _ := json.Marshal(map[string]any{"identity_pub": b64(a2.identity()),
		"envelopes": []any{map[string]any{"epoch": 1, "sealed": b64(env("x")[:40])}}})
	resp, _ = a.call("POST", "/v2/compass/rooms/"+room+"/successor", raw, "X-Compass-Linked-Token", a2.token(time.Now()))
	assert.Equal(t, http.StatusBadRequest, resp.StatusCode)
	resp, _ = succeed(a2, c2, "/successor")
	assert.Equal(t, http.StatusForbidden, resp.StatusCode, "a stranger, unlisted")

	// The owner, through /owner's successor form; B hears of it.
	sb, _ := b.subscribe(b.token(time.Now()))
	resp, text = succeed(a, a2, "/owner")
	require.Equal(t, http.StatusNoContent, resp.StatusCode, text)
	sb.expect("event: room", `data: {"room_id":"`+room+`","seq":1,"changes":["members"]}`, "")
	// The viewer, through /successor.
	resp, text = succeed(c, c2, "/successor")
	require.Equal(t, http.StatusNoContent, resp.StatusCode, text)

	view := a2.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, a2.id(), view["owner"])
	assert.Equal(t, "owner", view["role"])
	assert.Equal(t, []any{map[string]any{"epoch": 1.0, "sealed": b64(env("for " + a2.id()))}}, view["envelopes"])
	view = c2.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, "viewer", view["role"])
	out := c2.mustAPI(http.StatusOK, "GET", "/v2/compass/rooms/"+room+"/changes", nil)
	assert.Len(t, out["records"], 1, "the unlisted new chains use the room")
	code, out = a.api("GET", "/v2/compass/rooms/"+room, nil)
	assert.Equal(t, http.StatusForbidden, code)
	assert.Equal(t, "removed", out["error"])
	code, _ = c2.post(getUpdatesMsg(), c2.token(time.Now()))
	assert.Equal(t, http.StatusUnauthorized, code, "its personal sync still needs the list")

	// The old chains are cleared (reset): the room stays.
	db, err := datastore.NewSQLite(path, datastore.SQLiteOptions{})
	require.NoError(t, err)
	defer db.Close()
	for _, d := range []*device{a, c} {
		require.NoError(t, db.DisableSyncChain(context.Background(), d.chainID()))
		_, err = db.ClearServerData(context.Background(), d.chainID())
		require.NoError(t, err)
	}
	a2.mustAPI(http.StatusNoContent, "POST", "/v2/compass/rooms/"+room+"/epochs", map[string]any{"epoch": 2,
		"envelopes": []any{map[string]any{"client_id": a2.id(), "sealed": b64(env("2a"))},
			map[string]any{"client_id": b.id(), "sealed": b64(env("2b"))},
			map[string]any{"client_id": c2.id(), "sealed": b64(env("2c"))}}})
}
