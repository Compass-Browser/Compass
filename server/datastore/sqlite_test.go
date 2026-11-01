package datastore_test

// Tests for what the Compass SQLite store adds to go-sync's semantics.

import (
	"context"
	"errors"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
)

func openSQLite(t *testing.T, path string, maxBytes int64) *datastore.SQLite {
	t.Helper()
	db, err := datastore.NewSQLite(path, datastore.SQLiteOptions{MaxBytesPerChain: maxBytes})
	require.NoError(t, err)
	t.Cleanup(func() { db.Close() })
	return db
}

func entityAt(clientID, id string, mtime int64, specifics []byte) *datastore.SyncEntity {
	return &datastore.SyncEntity{
		ClientID:      clientID,
		ID:            id,
		Version:       ptr.Int64(mtime),
		Ctime:         ptr.Int64(mtime),
		Mtime:         ptr.Int64(mtime),
		DataType:      ptr.Int(123),
		Folder:        ptr.Bool(false),
		Deleted:       ptr.Bool(false),
		DataTypeMtime: ptr.String("123#" + strconv.FormatInt(mtime, 10)),
		Specifics:     specifics,
	}
}

func ids(entities []datastore.SyncEntity) []string {
	out := []string{}
	for _, e := range entities {
		out = append(out, e.ID)
	}
	return out
}

func TestSQLiteByteQuota(t *testing.T) {
	ctx := context.Background()
	db := openSQLite(t, filepath.Join(t.TempDir(), "q.db"), 100)

	_, err := db.InsertSyncEntity(ctx, entityAt("c1", "a", 1, make([]byte, 60)))
	require.NoError(t, err)
	_, err = db.InsertSyncEntity(ctx, entityAt("c1", "b", 2, make([]byte, 60)))
	require.ErrorIs(t, err, datastore.ErrQuotaExceeded)
	has, err := db.HasItem(ctx, "c1", "b")
	require.NoError(t, err)
	assert.False(t, has, "a refused insert writes nothing")

	// Another chain has its own quota.
	_, err = db.InsertSyncEntity(ctx, entityAt("c2", "b", 2, make([]byte, 60)))
	require.NoError(t, err)

	// Growing an entity past the quota fails; shrinking (a tombstone) works.
	grow := entityAt("c1", "a", 3, make([]byte, 120))
	_, _, err = db.UpdateSyncEntity(ctx, grow, 1)
	require.ErrorIs(t, err, datastore.ErrQuotaExceeded)
	tomb := entityAt("c1", "a", 3, []byte{1})
	tomb.Deleted = ptr.Bool(true)
	conflict, deleted, err := db.UpdateSyncEntity(ctx, tomb, 1)
	require.NoError(t, err)
	assert.False(t, conflict)
	assert.True(t, deleted)
	n, err := db.ChainBytes(ctx, "c1")
	require.NoError(t, err)
	assert.Equal(t, int64(1), n)

	// Freed space can be used again.
	_, err = db.InsertSyncEntity(ctx, entityAt("c1", "b", 4, make([]byte, 90)))
	require.NoError(t, err)

	// Clearing a chain resets its size.
	_, err = db.ClearServerData(ctx, "c1")
	require.NoError(t, err)
	n, err = db.ChainBytes(ctx, "c1")
	require.NoError(t, err)
	assert.Equal(t, int64(0), n)
}

func TestSQLiteGetUpdatesDoesNotSplitEqualMtimes(t *testing.T) {
	ctx := context.Background()
	db := openSQLite(t, filepath.Join(t.TempDir(), "t.db"), 0)
	for i, mtime := range []int64{100, 101, 101, 101, 102} {
		_, err := db.InsertSyncEntity(ctx, entityAt("c", "id"+strconv.Itoa(i), mtime, nil))
		require.NoError(t, err)
	}

	// The page would end inside the run of 101s: cut before it.
	more, got, err := db.GetUpdatesForType(ctx, 123, 0, true, "c", 2)
	require.NoError(t, err)
	assert.True(t, more)
	assert.Equal(t, []string{"id0"}, ids(got))

	// Only the run fits: nothing now, the caller asks again with more room.
	more, got, err = db.GetUpdatesForType(ctx, 123, 100, true, "c", 2)
	require.NoError(t, err)
	assert.True(t, more)
	assert.Empty(t, got)

	more, got, err = db.GetUpdatesForType(ctx, 123, 100, true, "c", 3)
	require.NoError(t, err)
	assert.True(t, more, "a full page reports more, as go-sync did")
	assert.Equal(t, []string{"id1", "id2", "id3"}, ids(got))

	more, got, err = db.GetUpdatesForType(ctx, 123, 101, true, "c", 3)
	require.NoError(t, err)
	assert.False(t, more)
	assert.Equal(t, []string{"id4"}, ids(got))
}

func TestSQLiteMtimesAreUniqueAndSurviveReopen(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "m.db")
	db := openSQLite(t, path, 0)

	last := int64(0)
	for range 50 {
		m := datastore.NextMtime()
		require.Greater(t, m, last)
		last = m
	}

	// A database written by a clock that ran ahead: new mtimes follow it.
	future := time.Now().Add(3 * time.Second).UnixMilli()
	_, err := db.InsertSyncEntity(ctx, entityAt("c", "x", future, nil))
	require.NoError(t, err)
	require.NoError(t, db.Close())
	openSQLite(t, path, 0)
	assert.Greater(t, datastore.NextMtime(), future)
}

func TestSQLitePurgeExpired(t *testing.T) {
	ctx := context.Background()
	db := openSQLite(t, filepath.Join(t.TempDir(), "p.db"), 0)
	now := time.Now()

	old := entityAt("c", "old", 1, make([]byte, 10))
	old.ExpirationTime = ptr.Int64(now.Add(-time.Minute).Unix())
	old.DataType = ptr.Int(datastore.HistoryDeleteDirectiveTypeID)
	old.ClientDefinedUniqueTag = ptr.String("tag-old")
	fresh := entityAt("c", "fresh", 2, make([]byte, 20))
	fresh.ExpirationTime = ptr.Int64(now.Add(time.Hour).Unix())
	keep := entityAt("c", "keep", 3, make([]byte, 30))
	for _, e := range []*datastore.SyncEntity{old, fresh, keep} {
		_, err := db.InsertSyncEntity(ctx, e)
		require.NoError(t, err)
	}

	n, err := db.PurgeExpired(ctx, now)
	require.NoError(t, err)
	assert.Equal(t, int64(1), n)
	all, err := db.AllSyncEntities(ctx)
	require.NoError(t, err)
	assert.Equal(t, []string{"fresh", "keep"}, ids(all))
	tags, err := db.AllTagItems(ctx)
	require.NoError(t, err)
	assert.Empty(t, tags, "the expired entity's client tag is freed")
	size, err := db.ChainBytes(ctx, "c")
	require.NoError(t, err)
	assert.Equal(t, int64(50), size)
}

func TestDataTypeOfSpecificsUnknownType(t *testing.T) {
	// A known type goes through the generated oneof.
	known := &sync_pb.EntitySpecifics{
		SpecificsVariant: &sync_pb.EntitySpecifics_Bookmark{Bookmark: &sync_pb.BookmarkSpecifics{}},
	}
	dt, err := datastore.DataTypeOfSpecifics(known)
	require.NoError(t, err)
	assert.Equal(t, 32904, dt)

	// A type newer than the server's protos (as COMPASS_SPACES will be), with
	// encrypted data in field 1 before it, as clients send it.
	const newType = 1234567
	var raw []byte
	raw = protowire.AppendTag(raw, 1, protowire.BytesType)
	raw = protowire.AppendBytes(raw, []byte{0x0a, 0x01, 'k'})
	raw = protowire.AppendTag(raw, newType, protowire.BytesType)
	raw = protowire.AppendBytes(raw, nil)
	unknown := &sync_pb.EntitySpecifics{}
	require.NoError(t, proto.Unmarshal(raw, unknown))
	dt, err = datastore.DataTypeOfSpecifics(unknown)
	require.NoError(t, err)
	assert.Equal(t, newType, dt)

	// The commit path stores it and gives it back byte for byte.
	pb := &sync_pb.SyncEntity{
		IdString: ptr.String("x"), Version: ptr.Int64(0), Specifics: unknown,
	}
	e, err := datastore.CreateDBSyncEntity(pb, ptr.String("guid"), "c")
	require.NoError(t, err)
	assert.Equal(t, newType, *e.DataType)
	back, err := datastore.CreatePBSyncEntity(e)
	require.NoError(t, err)
	out, err := proto.Marshal(back.Specifics)
	require.NoError(t, err)
	assert.Equal(t, raw, out)

	// No data type at all is an error, not a panic.
	_, err = datastore.DataTypeOfSpecifics(&sync_pb.EntitySpecifics{})
	require.Error(t, err)
	assert.False(t, errors.Is(err, datastore.ErrQuotaExceeded))
}

func TestSQLiteBackupTo(t *testing.T) {
	ctx := context.Background()
	dir := t.TempDir()
	db := openSQLite(t, filepath.Join(dir, "live.db"), 0)
	_, err := db.InsertSyncEntity(ctx, entityAt("c", "x", 5, []byte{1, 2, 3}))
	require.NoError(t, err)

	backup := filepath.Join(dir, "backup.db")
	require.NoError(t, db.BackupTo(ctx, backup))
	require.Error(t, db.BackupTo(ctx, backup), "won't overwrite")

	copyDB := openSQLite(t, backup, 0)
	all, err := copyDB.AllSyncEntities(ctx)
	require.NoError(t, err)
	assert.Equal(t, []string{"x"}, ids(all))
	n, err := copyDB.ChainBytes(ctx, "c")
	require.NoError(t, err)
	assert.Equal(t, int64(3), n)
}
