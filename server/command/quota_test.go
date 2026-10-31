package command_test

import (
	"context"
	"path/filepath"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/cache"
	"github.com/brave/go-sync/command"
	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
)

// Compass: the SQLite store's byte quota comes back as OVER_QUOTA for the
// entries that don't fit, and the others still commit.
func TestCommitOverByteQuota(t *testing.T) {
	db, err := datastore.NewSQLite(filepath.Join(t.TempDir(), "q.db"),
		datastore.SQLiteOptions{MaxBytesPerChain: 1000})
	require.NoError(t, err)
	defer db.Close()
	c := cache.NewCache(cache.NewMemoryClient())

	big := func(id string) *sync_pb.SyncEntity {
		e := getCommitEntity(id, 0, false, getBookmarkSpecifics())
		e.Name = ptr.String(string(make([]byte, 400)))
		return e
	}
	msg := getClientToServerCommitMsg([]*sync_pb.SyncEntity{big("a"), big("b"), big("c")})
	rsp := &sync_pb.ClientToServerResponse{}
	require.NoError(t, command.HandleClientToServerMessage(context.Background(), c, msg, rsp, db, "chain"))

	require.Len(t, rsp.Commit.Entryresponse, 3)
	assert.Equal(t, sync_pb.CommitResponse_SUCCESS, *rsp.Commit.Entryresponse[0].ResponseType)
	assert.Equal(t, sync_pb.CommitResponse_SUCCESS, *rsp.Commit.Entryresponse[1].ResponseType)
	assert.Equal(t, sync_pb.CommitResponse_OVER_QUOTA, *rsp.Commit.Entryresponse[2].ResponseType)
}
