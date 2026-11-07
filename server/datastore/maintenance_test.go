package datastore_test

// Tombstone purge, idle chains, quota state, schema 2, updates of purged
// entities, history retention (sync.md, "Server" and "History").

import (
	"context"
	"database/sql"
	"path/filepath"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/ptr"
)

func withTag(e *datastore.SyncEntity, tag string) *datastore.SyncEntity {
	e.ClientDefinedUniqueTag = ptr.String(tag)
	return e
}

func tombstoneAt(t *testing.T, db *datastore.SQLite, e *datastore.SyncEntity, mtime int64) {
	t.Helper()
	del := *e
	del.Version = ptr.Int64(mtime)
	del.Mtime = ptr.Int64(mtime)
	del.Deleted = ptr.Bool(true)
	del.Specifics = nil
	conflict, deleted, err := db.UpdateSyncEntity(context.Background(), &del, *e.Version)
	require.NoError(t, err)
	require.False(t, conflict)
	require.True(t, deleted)
}

func TestPurgeTombstones(t *testing.T) {
	ctx := context.Background()
	db := openSQLite(t, filepath.Join(t.TempDir(), "t.db"), 0)
	now := time.Now()
	old := now.AddDate(0, 0, -40).UnixMilli()
	recent := now.AddDate(0, 0, -5).UnixMilli()

	a := withTag(entityAt("c1", "a", old-10, []byte("aaaa")), "tag-a")
	b := withTag(entityAt("c1", "b", old-10, []byte("bbbb")), "tag-b")
	live := withTag(entityAt("c1", "live", old-10, []byte("live")), "tag-live")
	for _, e := range []*datastore.SyncEntity{a, b, live} {
		_, err := db.InsertSyncEntity(ctx, e)
		require.NoError(t, err)
	}
	tombstoneAt(t, db, a, old)    // Deleted 40 days ago: goes.
	tombstoneAt(t, db, b, recent) // Deleted 5 days ago: stays.

	n, err := db.PurgeTombstones(ctx, now.AddDate(0, 0, -30))
	require.NoError(t, err)
	assert.EqualValues(t, 1, n)

	all, err := db.AllSyncEntities(ctx)
	require.NoError(t, err)
	assert.Equal(t, []string{"b", "live"}, ids(all))

	// A create with the purged entity's tag makes a new entity.
	again := withTag(entityAt("c1", "a2", now.UnixMilli(), []byte("new")), "tag-a")
	_, err = db.InsertSyncEntity(ctx, again)
	require.NoError(t, err)
	assert.Equal(t, "a2", again.ID)
}

func TestUpdateOfPurgedEntity(t *testing.T) {
	ctx := context.Background()
	db := openSQLite(t, filepath.Join(t.TempDir(), "u.db"), 0)
	mtime := time.Now().UnixMilli()

	// Deleting what isn't there (a purged tombstone, an expired directive)
	// is done: no conflict.
	gone := withTag(entityAt("c1", "gone", mtime, nil), "tag-gone")
	gone.Deleted = ptr.Bool(true)
	conflict, deleted, err := db.UpdateSyncEntity(ctx, gone, 5)
	require.NoError(t, err)
	assert.False(t, conflict)
	assert.False(t, deleted)

	// An update of a purged entity with a client tag re-creates it.
	edit := withTag(entityAt("c1", "edited", mtime, []byte("edit")), "tag-edit")
	edit.Ctime = nil
	edit.Folder = nil
	conflict, _, err = db.UpdateSyncEntity(ctx, edit, 5)
	require.NoError(t, err)
	assert.False(t, conflict)
	has, err := db.HasItem(ctx, "c1", "edited")
	require.NoError(t, err)
	assert.True(t, has)
	_, updates, err := db.GetUpdatesForType(ctx, 123, 0, false, "c1", 10)
	require.NoError(t, err)
	assert.Equal(t, []string{"edited"}, ids(updates))

	// Without a client tag (old bookmarks) it stays a conflict.
	untagged := entityAt("c1", "untagged", mtime, []byte("x"))
	conflict, _, err = db.UpdateSyncEntity(ctx, untagged, 5)
	require.NoError(t, err)
	assert.True(t, conflict)
}

func TestTouchAndExpireIdleChains(t *testing.T) {
	ctx := context.Background()
	db := openSQLite(t, filepath.Join(t.TempDir(), "i.db"), 0)
	now := time.Now()

	_, err := db.InsertSyncEntity(ctx, entityAt("idle", "a", 1, []byte("x")))
	require.NoError(t, err)
	_, err = db.InsertSyncEntity(ctx, entityAt("busy", "b", 1, []byte("y")))
	require.NoError(t, err)
	// "untracked" stored data but never made a request since last_active
	// existed (NULL): it isn't expired.
	_, err = db.InsertSyncEntity(ctx, entityAt("untracked", "c", 1, []byte("z")))
	require.NoError(t, err)

	require.NoError(t, db.TouchChain(ctx, "idle", now.AddDate(0, 0, -200)))
	require.NoError(t, db.TouchChain(ctx, "busy", now.AddDate(0, 0, -200)))
	// Within the hour after a write, a touch writes nothing...
	require.NoError(t, db.TouchChain(ctx, "busy", now.AddDate(0, 0, -200).Add(time.Minute)))
	last, err := db.LastActive(ctx, "busy")
	require.NoError(t, err)
	assert.Equal(t, now.AddDate(0, 0, -200).Unix(), last.Unix())
	// ...later it does.
	require.NoError(t, db.TouchChain(ctx, "busy", now))
	last, err = db.LastActive(ctx, "busy")
	require.NoError(t, err)
	assert.Equal(t, now.Unix(), last.Unix())

	expired, err := db.ExpireIdleChains(ctx, now.AddDate(0, 0, -180))
	require.NoError(t, err)
	assert.Equal(t, []string{"idle"}, expired)

	disabled, err := db.IsSyncChainDisabled(ctx, "idle")
	require.NoError(t, err)
	assert.True(t, disabled)
	disabled, err = db.IsSyncChainDisabled(ctx, "busy")
	require.NoError(t, err)
	assert.False(t, disabled)
	all, err := db.AllSyncEntities(ctx)
	require.NoError(t, err)
	assert.Equal(t, []string{"b", "c"}, ids(all))
	n, err := db.ChainBytes(ctx, "idle")
	require.NoError(t, err)
	assert.Zero(t, n)

	// Expired chains aren't expired again.
	expired, err = db.ExpireIdleChains(ctx, now.AddDate(0, 0, -180))
	require.NoError(t, err)
	assert.Empty(t, expired)
}

func TestRecordQuota(t *testing.T) {
	ctx := context.Background()
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	t0 := time.Unix(1_800_000_000, 0)

	full, at, err := db.QuotaState(ctx, "c1")
	require.NoError(t, err)
	assert.Zero(t, full)
	assert.True(t, at.IsZero())

	require.NoError(t, db.RecordQuota(ctx, "c1", datastore.QuotaBytes|datastore.QuotaHistory, 0, t0))
	full, at, err = db.QuotaState(ctx, "c1")
	require.NoError(t, err)
	assert.Equal(t, datastore.QuotaBytes|datastore.QuotaHistory, full)
	assert.Equal(t, t0.Unix(), at.Unix())

	// A commit that stored a new entity but was refused history: bytes
	// clear, history stays (and its time moves).
	require.NoError(t, db.RecordQuota(ctx, "c1", datastore.QuotaHistory,
		datastore.QuotaBytes|datastore.QuotaEntities|datastore.QuotaHistory, t0.Add(time.Hour)))
	full, at, err = db.QuotaState(ctx, "c1")
	require.NoError(t, err)
	assert.Equal(t, datastore.QuotaHistory, full)
	assert.Equal(t, t0.Add(time.Hour).Unix(), at.Unix())

	require.NoError(t, db.RecordQuota(ctx, "c1", 0, datastore.QuotaHistory, t0))
	full, _, err = db.QuotaState(ctx, "c1")
	require.NoError(t, err)
	assert.Zero(t, full)
}

// A database made by schema 1 gets the new chains columns, and its chains
// a fresh last_active.
func TestMigrateSchema1(t *testing.T) {
	path := filepath.Join(t.TempDir(), "v1.db")
	raw, err := sql.Open("sqlite", "file:"+path)
	require.NoError(t, err)
	_, err = raw.Exec(`
		CREATE TABLE chains (
			client_id       TEXT PRIMARY KEY,
			bytes           INTEGER NOT NULL DEFAULT 0,
			disabled_reason TEXT,
			disabled_mtime  INTEGER,
			disabled_ctime  INTEGER
		) WITHOUT ROWID;
		INSERT INTO chains (client_id, bytes) VALUES ('old', 42);
		PRAGMA user_version = 1;`)
	require.NoError(t, err)
	require.NoError(t, raw.Close())

	db := openSQLite(t, path, 0)
	ctx := context.Background()
	last, err := db.LastActive(ctx, "old")
	require.NoError(t, err)
	assert.WithinDuration(t, time.Now(), last, time.Minute)
	n, err := db.ChainBytes(ctx, "old")
	require.NoError(t, err)
	assert.EqualValues(t, 42, n)
	require.NoError(t, db.RecordQuota(ctx, "old", datastore.QuotaEntities, 0, time.Now()))
	full, _, err := db.QuotaState(ctx, "old")
	require.NoError(t, err)
	assert.Equal(t, datastore.QuotaEntities, full)
	require.NoError(t, db.Close())

	// Opening it again changes nothing.
	db = openSQLite(t, path, 0)
	full, _, err = db.QuotaState(ctx, "old")
	require.NoError(t, err)
	assert.Equal(t, datastore.QuotaEntities, full)
}

func TestSetHistoryDays(t *testing.T) {
	t.Cleanup(func() { _ = datastore.SetHistoryDays(datastore.DefaultHistoryDays) })
	assert.EqualValues(t, 14*24*3600, datastore.HistoryExpirationIntervalSecs)
	require.NoError(t, datastore.SetHistoryDays(7))
	assert.EqualValues(t, 7*24*3600, datastore.HistoryExpirationIntervalSecs)
	assert.Error(t, datastore.SetHistoryDays(0))
	assert.Error(t, datastore.SetHistoryDays(datastore.MaxHistoryDays+1))
	assert.EqualValues(t, 7*24*3600, datastore.HistoryExpirationIntervalSecs)
}
