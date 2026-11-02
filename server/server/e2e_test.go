package server_test

// End to end over HTTP against the SQLite-backed server: two devices of one
// sync chain commit and get updates with real signed tokens, then pair a
// third device through the relay. Tokens are made as Compass makes them
// (sync.md, "Identity"; slice 0's MakeAccessToken): the Ed25519 key comes
// from HKDF-SHA256(seed, info "compass-sync-auth"), the token is
// base64url_padded(HEX(ts_ms) "|" HEX(sig) "|" HEX(pubkey)), upper-case hex.

import (
	"bytes"
	"crypto/ed25519"
	"crypto/hkdf"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	neturl "net/url"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
)

const (
	bookmarkTypeID = 32904
	nigoriTypeID   = 47745
	// A data type newer than the server's protos, as COMPASS_SPACES will be.
	futureTypeID = 1234567
)

// serverURL is the server under test: COMPASS_SYNC_E2E_URL (e.g.
// http://localhost:8296, to check a running server or container) or this
// package's router on a local listener.
func serverURL(t *testing.T) string {
	if url := os.Getenv("COMPASS_SYNC_E2E_URL"); url != "" {
		return strings.TrimSuffix(url, "/")
	}
	srv := httptest.NewServer(mux)
	t.Cleanup(srv.Close)
	return srv.URL
}

// randomSeed is a new chain's seed: tests can run again on a live server.
func randomSeed(t *testing.T) []byte {
	seed := make([]byte, 16)
	_, err := rand.Read(seed)
	require.NoError(t, err)
	return seed
}

type device struct {
	t         *testing.T
	url       string
	key       ed25519.PrivateKey
	cacheGUID string
	tokens    map[int32]int64
}

func newDevice(t *testing.T, url string, seed []byte, cacheGUID string) *device {
	priv, err := hkdf.Key(sha256.New, seed, nil, "compass-sync-auth", ed25519.SeedSize)
	require.NoError(t, err)
	return &device{t: t, url: url, key: ed25519.NewKeyFromSeed(priv), cacheGUID: cacheGUID,
		tokens: map[int32]int64{}}
}

func (d *device) chainID() string {
	return strings.ToUpper(hex.EncodeToString(d.key.Public().(ed25519.PublicKey)))
}

func (d *device) token(at time.Time) string {
	ts := []byte(strconv.FormatInt(at.UnixMilli(), 10))
	plain := strings.ToUpper(hex.EncodeToString(ts)) + "|" +
		strings.ToUpper(hex.EncodeToString(ed25519.Sign(d.key, ts))) + "|" + d.chainID()
	return base64.URLEncoding.EncodeToString([]byte(plain))
}

func (d *device) post(msg *sync_pb.ClientToServerMessage, token string) (int, *sync_pb.ClientToServerResponse) {
	d.t.Helper()
	msg.Share = ptr.String("")
	body, err := proto.Marshal(msg)
	require.NoError(d.t, err)
	req, err := http.NewRequest("POST", d.url+"/v2/command/?client=Compass&client_id="+d.cacheGUID,
		bytes.NewReader(body))
	require.NoError(d.t, err)
	req.Header.Set("Content-Type", "application/octet-stream")
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := http.DefaultClient.Do(req)
	require.NoError(d.t, err)
	defer resp.Body.Close()
	raw, err := io.ReadAll(resp.Body)
	require.NoError(d.t, err)
	if resp.StatusCode != http.StatusOK {
		return resp.StatusCode, nil
	}
	out := &sync_pb.ClientToServerResponse{}
	require.NoError(d.t, proto.Unmarshal(raw, out))
	return resp.StatusCode, out
}

func (d *device) do(msg *sync_pb.ClientToServerMessage) *sync_pb.ClientToServerResponse {
	d.t.Helper()
	code, rsp := d.post(msg, d.token(time.Now()))
	require.Equal(d.t, http.StatusOK, code)
	return rsp
}

func (d *device) getUpdates(origin sync_pb.SyncEnums_GetUpdatesOrigin, types ...int32) []*sync_pb.SyncEntity {
	d.t.Helper()
	var markers []*sync_pb.DataTypeProgressMarker
	for _, ty := range types {
		tok := make([]byte, binary.MaxVarintLen64)
		binary.PutVarint(tok, d.tokens[ty])
		markers = append(markers, &sync_pb.DataTypeProgressMarker{DataTypeId: ptr.Int32(ty), Token: tok})
	}
	contents := sync_pb.ClientToServerMessage_GET_UPDATES
	rsp := d.do(&sync_pb.ClientToServerMessage{
		MessageContents: &contents,
		GetUpdates: &sync_pb.GetUpdatesMessage{
			FetchFolders: ptr.Bool(true), FromProgressMarker: markers, GetUpdatesOrigin: &origin,
		},
	})
	require.Equal(d.t, sync_pb.SyncEnums_SUCCESS, rsp.GetErrorCode(), rsp.GetErrorMessage())
	for _, m := range rsp.GetUpdates.NewProgressMarker {
		v, n := binary.Varint(m.Token)
		require.Positive(d.t, n)
		d.tokens[m.GetDataTypeId()] = v
	}
	return rsp.GetUpdates.Entries
}

func (d *device) commit(entries ...*sync_pb.SyncEntity) []*sync_pb.CommitResponse_EntryResponse {
	d.t.Helper()
	contents := sync_pb.ClientToServerMessage_COMMIT
	rsp := d.do(&sync_pb.ClientToServerMessage{
		MessageContents: &contents,
		Commit:          &sync_pb.CommitMessage{Entries: entries, CacheGuid: ptr.String(d.cacheGUID)},
	})
	require.Equal(d.t, sync_pb.SyncEnums_SUCCESS, rsp.GetErrorCode(), rsp.GetErrorMessage())
	return rsp.Commit.Entryresponse
}

func bookmark(id, parent, title string, version int64) *sync_pb.SyncEntity {
	return &sync_pb.SyncEntity{
		IdString: ptr.String(id), ParentIdString: ptr.String(parent), Version: ptr.Int64(version),
		Name: ptr.String(title), Folder: ptr.Bool(false), Deleted: ptr.Bool(false),
		Specifics: &sync_pb.EntitySpecifics{SpecificsVariant: &sync_pb.EntitySpecifics_Bookmark{
			Bookmark: &sync_pb.BookmarkSpecifics{
				Url: ptr.String("https://example.com/" + title), FullTitle: ptr.String(title)},
		}},
	}
}

// futureSpecifics is EntitySpecifics with encrypted data (field 1) and an
// empty field of a type the server doesn't know, as an encrypted entity of
// a new type arrives.
func futureSpecifics(t *testing.T, blob []byte) (*sync_pb.EntitySpecifics, []byte) {
	enc, err := proto.Marshal(&sync_pb.EncryptedData{KeyName: ptr.String("k"), Blob: ptr.String(string(blob))})
	require.NoError(t, err)
	var raw []byte
	raw = protowire.AppendTag(raw, 1, protowire.BytesType)
	raw = protowire.AppendBytes(raw, enc)
	raw = protowire.AppendTag(raw, futureTypeID, protowire.BytesType)
	raw = protowire.AppendBytes(raw, nil)
	s := &sync_pb.EntitySpecifics{}
	require.NoError(t, proto.Unmarshal(raw, s))
	return s, raw
}

func findByTag(entries []*sync_pb.SyncEntity, tag string) *sync_pb.SyncEntity {
	for _, e := range entries {
		if e.GetServerDefinedUniqueTag() == tag {
			return e
		}
	}
	return nil
}

func TestEndToEndTwoDevices(t *testing.T) {
	url := serverURL(t)
	seed := randomSeed(t)
	a := newDevice(t, url, seed, "cache-guid-a")
	b := newDevice(t, url, seed, "cache-guid-b")
	require.Equal(t, a.chainID(), b.chainID(), "one seed, one chain")

	// A's first sync: the server creates the permanent folders.
	initial := a.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID, bookmarkTypeID)
	bar := findByTag(initial, "bookmark_bar")
	require.NotNil(t, bar, "bookmark bar folder")
	require.NotNil(t, findByTag(initial, "google_chrome_nigori"))

	// A commits a bookmark and an entity of an unknown (future) type.
	spec, specRaw := futureSpecifics(t, []byte("opaque ciphertext"))
	future := &sync_pb.SyncEntity{
		IdString: ptr.String("local-future-1"), Version: ptr.Int64(0), Name: ptr.String("encrypted"),
		ClientTagHash: ptr.String("future-tag-1"), Specifics: spec,
	}
	rsps := a.commit(bookmark("local-bm-1", bar.GetIdString(), "first", 0), future)
	require.Len(t, rsps, 2)
	for _, r := range rsps {
		require.Equal(t, sync_pb.CommitResponse_SUCCESS, r.GetResponseType(), r.GetErrorMessage())
	}
	bmID, bmVersion := rsps[0].GetIdString(), rsps[0].GetVersion()

	// B joins the chain and gets both, byte for byte for the unknown type.
	b.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID, bookmarkTypeID)
	b.tokens = map[int32]int64{}
	got := b.getUpdates(sync_pb.SyncEnums_GU_TRIGGER, bookmarkTypeID, futureTypeID)
	var gotBM, gotFuture *sync_pb.SyncEntity
	for _, e := range got {
		switch e.GetIdString() {
		case bmID:
			gotBM = e
		case rsps[1].GetIdString():
			gotFuture = e
		}
	}
	require.NotNil(t, gotBM)
	assert.Equal(t, "https://example.com/first", gotBM.GetSpecifics().GetBookmark().GetUrl())
	assert.Equal(t, "cache-guid-a", gotBM.GetOriginatorCacheGuid())
	assert.Equal(t, bar.GetIdString(), gotBM.GetParentIdString())
	require.NotNil(t, gotFuture)
	rawBack, err := proto.Marshal(gotFuture.GetSpecifics())
	require.NoError(t, err)
	assert.Equal(t, specRaw, rawBack)
	assert.Equal(t, "future-tag-1", gotFuture.GetClientTagHash())

	// B renames the bookmark; A's next poll sees it, and only it.
	update := bookmark(bmID, bar.GetIdString(), "renamed", bmVersion)
	urs := b.commit(update)
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, urs[0].GetResponseType())
	assert.Greater(t, urs[0].GetVersion(), bmVersion)
	got = a.getUpdates(sync_pb.SyncEnums_PERIODIC, bookmarkTypeID)
	require.Len(t, got, 1)
	assert.Equal(t, "https://example.com/renamed", got[0].GetSpecifics().GetBookmark().GetUrl())

	// A's stale write conflicts.
	stale := a.commit(bookmark(bmID, bar.GetIdString(), "stale", bmVersion))
	assert.Equal(t, sync_pb.CommitResponse_CONFLICT, stale[0].GetResponseType())

	// A deletes it; B gets the tombstone.
	tomb := bookmark(bmID, bar.GetIdString(), "renamed", urs[0].GetVersion())
	tomb.Deleted = ptr.Bool(true)
	drs := a.commit(tomb)
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, drs[0].GetResponseType())
	got = b.getUpdates(sync_pb.SyncEnums_PERIODIC, bookmarkTypeID)
	require.Len(t, got, 1)
	assert.True(t, got[0].GetDeleted())

	// A batch bigger than one millisecond's worth arrives whole, page by page.
	var batch []*sync_pb.SyncEntity
	for i := range 90 {
		batch = append(batch, bookmark("local-batch-"+strconv.Itoa(i), bar.GetIdString(), "b"+strconv.Itoa(i), 0))
	}
	for _, r := range a.commit(batch...) {
		require.Equal(t, sync_pb.CommitResponse_SUCCESS, r.GetResponseType())
	}
	got = b.getUpdates(sync_pb.SyncEnums_PERIODIC, bookmarkTypeID)
	assert.Len(t, got, 90)
}

func TestEndToEndCreateOverTombstone(t *testing.T) {
	// A deletes an entity of the future type; B, which saw the delete, puts
	// it back with the same client tag (a space re-created under its old
	// ID): it comes back under its server ID, and A gets it undeleted.
	url := serverURL(t)
	seed := randomSeed(t)
	a := newDevice(t, url, seed, "cache-guid-a")
	b := newDevice(t, url, seed, "cache-guid-b")
	a.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID, bookmarkTypeID)

	spec, _ := futureSpecifics(t, []byte("v1"))
	entity := func(id string, version int64, s *sync_pb.EntitySpecifics) *sync_pb.SyncEntity {
		return &sync_pb.SyncEntity{IdString: ptr.String(id), Version: ptr.Int64(version),
			ClientTagHash: ptr.String("space-tag"), Specifics: s}
	}
	rs := a.commit(entity("local-a", 0, spec))
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, rs[0].GetResponseType())
	serverID := rs[0].GetIdString()
	tomb := entity(serverID, rs[0].GetVersion(), spec)
	tomb.Deleted = ptr.Bool(true)
	rs = a.commit(tomb)
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, rs[0].GetResponseType())
	got := b.getUpdates(sync_pb.SyncEnums_GU_TRIGGER, futureTypeID)
	require.Len(t, got, 1)
	require.True(t, got[0].GetDeleted())
	a.getUpdates(sync_pb.SyncEnums_PERIODIC, futureTypeID)

	spec2, raw2 := futureSpecifics(t, []byte("v2"))
	rs = b.commit(entity("local-b", 0, spec2))
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, rs[0].GetResponseType(), rs[0].GetErrorMessage())
	assert.Equal(t, serverID, rs[0].GetIdString())

	got = a.getUpdates(sync_pb.SyncEnums_PERIODIC, futureTypeID)
	require.Len(t, got, 1)
	assert.Equal(t, serverID, got[0].GetIdString())
	assert.False(t, got[0].GetDeleted())
	back, err := proto.Marshal(got[0].GetSpecifics())
	require.NoError(t, err)
	assert.Equal(t, raw2, back)

	// A, not knowing yet (or having lost track of it), creates it too: the
	// create replaces it under the same ID, as Chromium's servers do; a
	// CONFLICT would never clear, since A can't learn of the entity again.
	rs = a.commit(entity("local-a2", 0, spec))
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, rs[0].GetResponseType(), rs[0].GetErrorMessage())
	assert.Equal(t, serverID, rs[0].GetIdString())
	got = b.getUpdates(sync_pb.SyncEnums_PERIODIC, futureTypeID)
	require.Len(t, got, 1)
	back, err = proto.Marshal(got[0].GetSpecifics())
	require.NoError(t, err)
	_, raw1 := futureSpecifics(t, []byte("v1"))
	assert.Equal(t, raw1, back)
}

func TestEndToEndAuth(t *testing.T) {
	url := serverURL(t)
	d := newDevice(t, url, randomSeed(t), "guid")
	contents := sync_pb.ClientToServerMessage_GET_UPDATES
	gu := func() *sync_pb.ClientToServerMessage {
		return &sync_pb.ClientToServerMessage{MessageContents: &contents, GetUpdates: &sync_pb.GetUpdatesMessage{}}
	}

	code, _ := d.post(gu(), d.token(time.Now()))
	assert.Equal(t, http.StatusOK, code)
	code, _ = d.post(gu(), d.token(time.Now().Add(-25*time.Hour)))
	assert.Equal(t, http.StatusUnauthorized, code, "outside go-sync's ±24 h window")
	code, _ = d.post(gu(), "bm90IGEgdG9rZW4=")
	assert.Equal(t, http.StatusUnauthorized, code)
	other := newDevice(t, url, randomSeed(t), "guid")
	forged := strings.Replace(mustDecode(t, d.token(time.Now())), d.chainID(), other.chainID(), 1)
	code, _ = d.post(gu(), base64.URLEncoding.EncodeToString([]byte(forged)))
	assert.Equal(t, http.StatusUnauthorized, code, "a signature from another key")

	// "Reset sync": ClearServerData disables the chain.
	clear := sync_pb.ClientToServerMessage_CLEAR_SERVER_DATA
	rsp := d.do(&sync_pb.ClientToServerMessage{
		MessageContents: &clear, ClearServerData: &sync_pb.ClearServerDataMessage{}})
	assert.Equal(t, sync_pb.SyncEnums_SUCCESS, rsp.GetErrorCode())
	rsp = d.do(gu())
	assert.Equal(t, sync_pb.SyncEnums_DISABLED_BY_ADMIN, rsp.GetErrorCode())
}

func mustDecode(t *testing.T, token string) string {
	raw, err := base64.URLEncoding.DecodeString(token)
	require.NoError(t, err)
	return string(raw)
}

// Two fake clients pair through the real routes and real sync tokens.
func TestEndToEndPairing(t *testing.T) {
	url := serverURL(t)
	a := newDevice(t, url, randomSeed(t), "guid-a")
	// The canonical name: the host the server was reached at, unless it's
	// configured (COMPASS_SYNC_PUBLIC_HOST; give it as
	// COMPASS_SYNC_E2E_SERVER_NAME when testing such a server).
	wantName := os.Getenv("COMPASS_SYNC_E2E_SERVER_NAME")
	if wantName == "" {
		parsed, err := neturl.Parse(url)
		require.NoError(t, err)
		wantName = strings.ToLower(parsed.Hostname())
	}

	call := func(method, path string, hdr map[string]string, body any) (int, map[string]any) {
		var rd io.Reader = http.NoBody
		if body != nil {
			raw, _ := json.Marshal(body)
			rd = bytes.NewReader(raw)
		}
		req, err := http.NewRequest(method, url+"/v2/compass/pair"+path, rd)
		require.NoError(t, err)
		for k, v := range hdr {
			req.Header.Set(k, v)
		}
		resp, err := http.DefaultClient.Do(req)
		require.NoError(t, err)
		defer resp.Body.Close()
		var out map[string]any
		_ = json.NewDecoder(resp.Body).Decode(&out)
		return resp.StatusCode, out
	}
	aAuth := func() map[string]string { return map[string]string{"Authorization": "Bearer " + a.token(time.Now())} }
	b64 := func(s string) string { return base64.StdEncoding.EncodeToString([]byte(s)) }

	code, out := call("POST", "", aAuth(), nil)
	require.Equal(t, http.StatusCreated, code)
	slot := "/" + strconv.Itoa(int(out["slot"].(float64)))
	assert.EqualValues(t, 300, out["expires_in"])
	assert.Equal(t, wantName, out["server"])

	code, out = call("POST", slot+"/join", nil, nil)
	require.Equal(t, http.StatusOK, code)
	assert.Equal(t, wantName, out["server"])
	assert.NotZero(t, out["expires_in"])
	bAuth := map[string]string{"X-Compass-Pair-Token": out["b_token"].(string)}

	// 1. B -> A, SPAKE2 (Bob). A was already waiting.
	aGot := make(chan map[string]any)
	go func() {
		_, out := call("GET", slot+"/msg?for=a&after=0", aAuth(), nil)
		aGot <- out
	}()
	time.Sleep(50 * time.Millisecond)
	code, out = call("POST", slot+"/msg", bAuth, map[string]string{"from": "b", "data": b64("bob")})
	require.Equal(t, http.StatusOK, code)
	assert.EqualValues(t, 1, out["seq"])
	got := <-aGot
	require.Len(t, got["messages"], 1)
	assert.Equal(t, b64("bob"), got["messages"].([]any)[0].(map[string]any)["data"])

	// 2. A -> B, SPAKE2 (Alice).
	code, _ = call("POST", slot+"/msg", aAuth(), map[string]string{"from": "a", "data": b64("alice")})
	require.Equal(t, http.StatusOK, code)
	code, out = call("GET", slot+"/msg?for=b&after=0", bAuth, nil)
	require.Equal(t, http.StatusOK, code)
	assert.Equal(t, b64("alice"), out["messages"].([]any)[0].(map[string]any)["data"])

	// A closes (a wrong code, say); the slot is gone for both, and says why.
	code, _ = call("DELETE", slot+"?reason=mismatch", aAuth(), nil)
	assert.Equal(t, http.StatusNoContent, code)
	code, out = call("GET", slot+"/msg?for=b&after=1", bAuth, nil)
	assert.Equal(t, http.StatusGone, code)
	assert.Equal(t, "mismatch", out["reason"])
}
