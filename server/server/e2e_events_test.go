package server_test

// Change notifications end to end (sync.md, "Slice 6 notes"): devices of a
// chain hold /v2/compass/events open with their sync token; another
// device's commit sends them the changed types.

import (
	"bufio"
	"context"
	"net/http"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
)

type eventStream struct {
	t     *testing.T
	lines chan string
}

func (d *device) subscribe(token string) (*eventStream, int) {
	d.t.Helper()
	ctx, cancel := context.WithCancel(context.Background())
	d.t.Cleanup(cancel)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet,
		d.url+"/v2/compass/events?client_id="+d.cacheGUID, nil)
	require.NoError(d.t, err)
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := http.DefaultClient.Do(req)
	require.NoError(d.t, err)
	if resp.StatusCode != http.StatusOK {
		resp.Body.Close()
		return nil, resp.StatusCode
	}
	assert.Equal(d.t, "text/event-stream", resp.Header.Get("Content-Type"))
	s := &eventStream{t: d.t, lines: make(chan string, 100)}
	go func() {
		defer close(s.lines)
		defer resp.Body.Close()
		sc := bufio.NewScanner(resp.Body)
		for sc.Scan() {
			s.lines <- sc.Text()
		}
	}()
	s.expect(": connected", "")
	return s, http.StatusOK
}

func (s *eventStream) expect(lines ...string) {
	s.t.Helper()
	for _, want := range lines {
		select {
		case got, ok := <-s.lines:
			require.True(s.t, ok, "stream ended, wanted %q", want)
			require.Equal(s.t, want, got)
		case <-time.After(5 * time.Second):
			s.t.Fatalf("no %q in 5 s", want)
		}
	}
}

func (s *eventStream) quiet() {
	s.t.Helper()
	select {
	case l := <-s.lines:
		s.t.Fatalf("unexpected line %q", l)
	case <-time.After(200 * time.Millisecond):
	}
}

func TestEndToEndChangeNotifications(t *testing.T) {
	url := serverURL(t)
	seed := randomSeed(t)
	a := newDevice(t, url, seed, "cache-guid-a")
	b := newDevice(t, url, seed, "cache-guid-b")
	stranger := newDevice(t, url, randomSeed(t), "cache-guid-s")

	initial := a.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID, bookmarkTypeID)
	bar := findByTag(initial, "bookmark_bar")
	require.NotNil(t, bar)

	sa, _ := a.subscribe(a.token(time.Now()))
	sb, _ := b.subscribe(b.token(time.Now()))
	ss, _ := stranger.subscribe(stranger.token(time.Now()))

	// A commits a bookmark and an entity of a type the server's protos
	// don't know (as COMPASS_SPACES): B hears both types, A (the committer)
	// and the other chain nothing.
	spec, _ := futureSpecifics(t, []byte("x"))
	rsps := a.commit(bookmark("local-1", bar.GetIdString(), "one", 0),
		&sync_pb.SyncEntity{IdString: ptr.String("local-f"), Version: ptr.Int64(0),
			ClientTagHash: ptr.String("f-tag"), Specifics: spec})
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, rsps[0].GetResponseType())
	sb.expect("event: changed", `data: {"types":[32904,1234567]}`, "")
	sa.quiet()
	ss.quiet()

	// B commits: A hears it.
	b.commit(bookmark("local-2", bar.GetIdString(), "two", 0))
	sa.expect("event: changed", `data: {"types":[32904]}`, "")
	sb.quiet()

	// A commit that stores nothing (a conflict) tells nobody.
	stale := a.commit(bookmark(rsps[0].GetIdString(), bar.GetIdString(), "stale", rsps[0].GetVersion()-1))
	require.Equal(t, sync_pb.CommitResponse_CONFLICT, stale[0].GetResponseType())
	sb.quiet()

	// GetUpdates tells nobody either.
	a.getUpdates(sync_pb.SyncEnums_PERIODIC, bookmarkTypeID)
	sb.quiet()
}

func TestEndToEndEventsAuth(t *testing.T) {
	url := serverURL(t)
	d := newDevice(t, url, randomSeed(t), "guid")
	_, code := d.subscribe(d.token(time.Now().Add(-25 * time.Hour)))
	assert.Equal(t, http.StatusUnauthorized, code)
	_, code = d.subscribe("not-a-token")
	assert.Equal(t, http.StatusUnauthorized, code)
	_, code = d.subscribe(d.token(time.Now()))
	assert.Equal(t, http.StatusOK, code)
}

func TestPollIntervalIsLong(t *testing.T) {
	// With change notifications, polling is the fallback: 15 min by default.
	url := serverURL(t)
	d := newDevice(t, url, randomSeed(t), "guid")
	contents := sync_pb.ClientToServerMessage_GET_UPDATES
	rsp := d.do(&sync_pb.ClientToServerMessage{MessageContents: &contents, GetUpdates: &sync_pb.GetUpdatesMessage{}})
	assert.Equal(t, int32(900), rsp.GetClientCommand().GetSetSyncPollInterval())
}
