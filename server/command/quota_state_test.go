package command_test

// Slice 7 (sync.md, "Slice 7 notes"): commits refused for a quota are
// remembered per chain (datastore.RecordQuota), and the history settings
// come from the environment.

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

func historyEntity(tag string) *sync_pb.SyncEntity {
	return &sync_pb.SyncEntity{
		IdString:      ptr.String(tag),
		Version:       ptr.Int64(0),
		Deleted:       ptr.Bool(false),
		Folder:        ptr.Bool(false),
		ClientTagHash: ptr.String(tag),
		Specifics: &sync_pb.EntitySpecifics{
			SpecificsVariant: &sync_pb.EntitySpecifics_History{History: &sync_pb.HistorySpecifics{}},
		},
	}
}

func commitAll(t *testing.T, db *datastore.SQLite, c *cache.Cache, entries ...*sync_pb.SyncEntity) []*sync_pb.CommitResponse_EntryResponse {
	t.Helper()
	rsp := &sync_pb.ClientToServerResponse{}
	require.NoError(t, command.HandleClientToServerMessage(context.Background(), c,
		getClientToServerCommitMsg(entries), rsp, db, "chain"))
	return rsp.Commit.Entryresponse
}

func TestQuotaRefusalsRecorded(t *testing.T) {
	ctx := context.Background()
	db, err := datastore.NewSQLite(filepath.Join(t.TempDir(), "q.db"),
		datastore.SQLiteOptions{MaxBytesPerChain: 1000})
	require.NoError(t, err)
	defer db.Close()
	c := cache.NewCache(cache.NewMemoryClient())

	saved := *command.MaxClientHistoryQuota
	*command.MaxClientHistoryQuota = 2
	t.Cleanup(func() { *command.MaxClientHistoryQuota = saved })

	big := func(id string) *sync_pb.SyncEntity {
		e := getCommitEntity(id, 0, false, getBookmarkSpecifics())
		e.Name = ptr.String(string(make([]byte, 400)))
		return e
	}

	// Two fit, the third is refused: bytes.
	rsps := commitAll(t, db, c, big("a"), big("b"), big("c"))
	assert.Equal(t, sync_pb.CommitResponse_OVER_QUOTA, rsps[2].GetResponseType())
	full, at, err := db.QuotaState(ctx, "chain")
	require.NoError(t, err)
	assert.Equal(t, datastore.QuotaBytes, full)
	assert.False(t, at.IsZero())

	// History past its quota is dropped with SUCCESS, and remembered.
	rsps = commitAll(t, db, c, historyEntity("h1"), historyEntity("h2"), historyEntity("h3"))
	for _, r := range rsps {
		assert.Equal(t, sync_pb.CommitResponse_SUCCESS, r.GetResponseType())
	}
	full, _, err = db.QuotaState(ctx, "chain")
	require.NoError(t, err)
	assert.Equal(t, datastore.QuotaBytes|datastore.QuotaHistory, full)
	all, err := db.AllSyncEntities(ctx)
	require.NoError(t, err)
	history := 0
	for _, e := range all {
		if *e.DataType == datastore.HistoryTypeID {
			history++
		}
	}
	assert.Equal(t, 2, history)

	// A new entity that fits clears the byte flag; history stays full.
	small := getCommitEntity("d", 0, false, getBookmarkSpecifics())
	rsps = commitAll(t, db, c, small)
	assert.Equal(t, sync_pb.CommitResponse_SUCCESS, rsps[0].GetResponseType())
	full, _, err = db.QuotaState(ctx, "chain")
	require.NoError(t, err)
	assert.Equal(t, datastore.QuotaHistory, full)
}

func TestEntityQuotaRecorded(t *testing.T) {
	ctx := context.Background()
	db, err := datastore.NewSQLite(filepath.Join(t.TempDir(), "e.db"), datastore.SQLiteOptions{})
	require.NoError(t, err)
	defer db.Close()
	c := cache.NewCache(cache.NewMemoryClient())

	saved := *command.MaxClientObjectQuota
	*command.MaxClientObjectQuota = 1
	t.Cleanup(func() { *command.MaxClientObjectQuota = saved })

	rsps := commitAll(t, db, c,
		getCommitEntity("a", 0, false, getBookmarkSpecifics()),
		getCommitEntity("b", 0, false, getBookmarkSpecifics()))
	assert.Equal(t, sync_pb.CommitResponse_SUCCESS, rsps[0].GetResponseType())
	assert.Equal(t, sync_pb.CommitResponse_OVER_QUOTA, rsps[1].GetResponseType())
	full, _, err := db.QuotaState(ctx, "chain")
	require.NoError(t, err)
	assert.Equal(t, datastore.QuotaEntities, full)
}

// A device that deletes an entity the server no longer has (a purged
// tombstone, an expired history delete directive) gets SUCCESS, not a
// CONFLICT it would retry forever.
func TestDeleteOfMissingEntitySucceeds(t *testing.T) {
	db, err := datastore.NewSQLite(filepath.Join(t.TempDir(), "m.db"), datastore.SQLiteOptions{})
	require.NoError(t, err)
	defer db.Close()
	c := cache.NewCache(cache.NewMemoryClient())

	directive := &sync_pb.SyncEntity{
		IdString:      ptr.String("server-id-of-a-purged-directive"),
		Version:       ptr.Int64(1234),
		Deleted:       ptr.Bool(true),
		ClientTagHash: ptr.String("directive-tag"),
		Specifics: &sync_pb.EntitySpecifics{
			SpecificsVariant: &sync_pb.EntitySpecifics_HistoryDeleteDirective{
				HistoryDeleteDirective: &sync_pb.HistoryDeleteDirectiveSpecifics{},
			},
		},
	}
	rsps := commitAll(t, db, c, directive)
	assert.Equal(t, sync_pb.CommitResponse_SUCCESS, rsps[0].GetResponseType())
}

func TestLoadQuotaFromEnv(t *testing.T) {
	t.Cleanup(func() {
		t.Setenv("COMPASS_SYNC_MAX_ENTITIES", "")
		t.Setenv("COMPASS_SYNC_MAX_HISTORY", "")
		t.Setenv("COMPASS_SYNC_HISTORY_DAYS", "")
		command.LoadQuotaFromEnv()
	})

	command.LoadQuotaFromEnv()
	assert.Equal(t, command.Quotas{MaxEntities: 100000, MaxHistory: 30000, HistoryDays: 14},
		command.CurrentQuotas())

	t.Setenv("COMPASS_SYNC_MAX_HISTORY", "50000")
	t.Setenv("COMPASS_SYNC_HISTORY_DAYS", "28")
	command.LoadQuotaFromEnv()
	assert.Equal(t, command.Quotas{MaxEntities: 100000, MaxHistory: 50000, HistoryDays: 28},
		command.CurrentQuotas())

	// History can't take more than the whole; invalid values are ignored.
	t.Setenv("COMPASS_SYNC_MAX_ENTITIES", "20000")
	t.Setenv("COMPASS_SYNC_HISTORY_DAYS", "90")
	command.LoadQuotaFromEnv()
	assert.Equal(t, command.Quotas{MaxEntities: 20000, MaxHistory: 20000, HistoryDays: 14},
		command.CurrentQuotas())

	t.Setenv("COMPASS_SYNC_MAX_HISTORY", "zero")
	t.Setenv("COMPASS_SYNC_MAX_ENTITIES", "")
	t.Setenv("COMPASS_SYNC_HISTORY_DAYS", "")
	command.LoadQuotaFromEnv()
	assert.Equal(t, command.Quotas{MaxEntities: 100000, MaxHistory: 30000, HistoryDays: 14},
		command.CurrentQuotas())
}
