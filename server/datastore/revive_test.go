package datastore_test

// A create (version 0) whose client tag belongs to a deleted entity revives
// that entity under its ID, as Chromium's servers do; one whose tag belongs
// to a live entity is a conflict.

import (
	"context"
	"database/sql"
	"path/filepath"
	"strconv"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/ptr"
)

const (
	passwordType = 45873
	// COMPASS_SPACES' field number, a type the server's protos don't have.
	compassSpacesType = 970000
)

// tagged is a create of a client-tagged entity, as CreateDBSyncEntity makes
// one (version = mtime).
func tagged(id, tag string, dataType int, specifics []byte) *datastore.SyncEntity {
	mtime := datastore.NextMtime()
	return &datastore.SyncEntity{
		ClientID:               "c1",
		ID:                     id,
		Version:                ptr.Int64(mtime),
		Ctime:                  ptr.Int64(mtime),
		Mtime:                  ptr.Int64(mtime),
		DataType:               ptr.Int(dataType),
		Folder:                 ptr.Bool(false),
		Deleted:                ptr.Bool(false),
		DataTypeMtime:          ptr.String(strconv.Itoa(dataType) + "#" + strconv.FormatInt(mtime, 10)),
		Specifics:              specifics,
		ClientDefinedUniqueTag: ptr.String(tag),
		OriginatorCacheGUID:    ptr.String("guid"),
		OriginatorClientItemID: ptr.String(id),
	}
}

// deleteEntity commits a tombstone for e (withTag: as Chromium sends it, with
// the client tag) and returns its version.
func deleteEntity(t *testing.T, db *datastore.SQLite, e *datastore.SyncEntity, withTag bool) int64 {
	t.Helper()
	tomb := tagged(e.ID, *e.ClientDefinedUniqueTag, *e.DataType, []byte{1})
	if !withTag {
		tomb.ClientDefinedUniqueTag = nil
	}
	tomb.Deleted = ptr.Bool(true)
	conflict, deleted, err := db.UpdateSyncEntity(context.Background(), tomb, *e.Version)
	require.NoError(t, err)
	require.False(t, conflict)
	require.True(t, deleted)
	return *tomb.Version
}

func entityByID(t *testing.T, db *datastore.SQLite, id string) []datastore.SyncEntity {
	t.Helper()
	all, err := db.AllSyncEntities(context.Background())
	require.NoError(t, err)
	var out []datastore.SyncEntity
	for _, e := range all {
		if e.ID == id {
			out = append(out, e)
		}
	}
	return out
}

func TestSQLiteCreateOverTombstoneRevives(t *testing.T) {
	for _, tc := range []struct {
		name     string
		dataType int
		withTag  bool
	}{
		{"password", passwordType, true},
		{"compass spaces", compassSpacesType, true},
		// A delete committed without the tag left the tag item behind: the
		// case that answered CONFLICT forever.
		{"tag item left over", compassSpacesType, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			ctx := context.Background()
			db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)

			first := tagged("server-id-1", "tag", tc.dataType, make([]byte, 40))
			conflict, err := db.InsertSyncEntity(ctx, first)
			require.NoError(t, err)
			require.False(t, conflict)
			deletedAt := deleteEntity(t, db, first, tc.withTag)
			bytes, err := db.ChainBytes(ctx, "c1")
			require.NoError(t, err)
			assert.Equal(t, int64(1), bytes, "a tombstone keeps its specifics' bytes")

			again := tagged("server-id-2", "tag", tc.dataType, make([]byte, 70))
			again.Name = ptr.String("again")
			conflict, err = db.InsertSyncEntity(ctx, again)
			require.NoError(t, err)
			require.False(t, conflict)
			assert.Equal(t, "server-id-1", again.ID, "the tag's server ID")

			assert.Empty(t, entityByID(t, db, "server-id-2"))
			got := entityByID(t, db, "server-id-1")
			require.Len(t, got, 1)
			e := got[0]
			assert.False(t, *e.Deleted)
			assert.Greater(t, *e.Version, deletedAt)
			assert.Equal(t, *again.Version, *e.Version)
			assert.Equal(t, *again.Mtime, *e.Mtime)
			assert.Equal(t, *again.Ctime, *e.Ctime)
			assert.Len(t, e.Specifics, 70)
			assert.Equal(t, "again", *e.Name)
			assert.Equal(t, "server-id-2", *e.OriginatorClientItemID)

			bytes, err = db.ChainBytes(ctx, "c1")
			require.NoError(t, err)
			assert.Equal(t, int64(70+len("again")), bytes)
			tags, err := db.AllTagItems(ctx)
			require.NoError(t, err)
			require.Len(t, tags, 1)
			assert.Equal(t, "Client#tag", tags[0].ID)

			// Another device gets it back undeleted, under its ID.
			_, updates, err := db.GetUpdatesForType(ctx, tc.dataType, deletedAt, true, "c1", 100)
			require.NoError(t, err)
			require.Len(t, updates, 1)
			assert.Equal(t, "server-id-1", updates[0].ID)
			assert.False(t, *updates[0].Deleted)

			// The revived entity's tag is taken again: another create
			// replaces it, under its ID.
			live := tagged("server-id-3", "tag", tc.dataType, []byte{1})
			conflict, err = db.InsertSyncEntity(ctx, live)
			require.NoError(t, err)
			assert.False(t, conflict)
			assert.Equal(t, "server-id-1", live.ID)
		})
	}
}

func TestSQLiteCreateOverLiveTagReplaces(t *testing.T) {
	// Two devices created the entity of one tag (the icon of one host), or
	// one lost track of it: the create replaces it, the last writer wins.
	ctx := context.Background()
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	_, err := db.InsertSyncEntity(ctx, tagged("a", "tag", passwordType, []byte{1, 2}))
	require.NoError(t, err)

	create := tagged("b", "tag", passwordType, []byte{1, 2, 3})
	conflict, err := db.InsertSyncEntity(ctx, create)
	require.NoError(t, err)
	assert.False(t, conflict)
	assert.Equal(t, "a", create.ID)
	assert.Empty(t, entityByID(t, db, "b"))
	bytes, err := db.ChainBytes(ctx, "c1")
	require.NoError(t, err)
	assert.Equal(t, int64(3), bytes, "the new size replaces the old")

	// A delete committed as a create (a tombstone of an entity this device
	// never had) doesn't delete the live one.
	tomb := tagged("d", "tag", passwordType, nil)
	tomb.Deleted = ptr.Bool(true)
	conflict, err = db.InsertSyncEntity(ctx, tomb)
	require.Error(t, err)
	assert.True(t, conflict)

	// The same tag in another data type is another entity.
	conflict, err = db.InsertSyncEntity(ctx, tagged("c", "other-tag", compassSpacesType, []byte{1}))
	require.NoError(t, err)
	assert.False(t, conflict)
}

func TestSQLiteCreateOverTombstoneKeepsTheLatest(t *testing.T) {
	// Before reviving, go-sync gave a create over a tombstone a new ID: one
	// tag, a tombstone and a live entity. The live one holds the tag.
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "r.db")
	db := openSQLite(t, path, 0)
	old := tagged("old", "tag", compassSpacesType, []byte{1})
	_, err := db.InsertSyncEntity(ctx, old)
	require.NoError(t, err)
	deleteEntity(t, db, old, true)
	raw, err := sql.Open("sqlite", path)
	require.NoError(t, err)
	defer raw.Close()
	_, err = raw.ExecContext(ctx, `INSERT INTO entities (client_id, id, version, mtime, ctime, deleted,
		data_type, folder, client_defined_unique_tag, specifics, size)
		VALUES ('c1', 'new', ?1, ?1, ?1, 0, ?2, 0, 'tag', x'01', 1)`,
		datastore.NextMtime(), compassSpacesType)
	require.NoError(t, err)

	// A create then replaces the live one, the latest.
	create := tagged("x", "tag", compassSpacesType, []byte{2})
	conflict, err := db.InsertSyncEntity(ctx, create)
	require.NoError(t, err)
	assert.False(t, conflict)
	assert.Equal(t, "new", create.ID)
}

func TestSQLiteCreateOverTombstoneQuota(t *testing.T) {
	ctx := context.Background()
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 100)
	first := tagged("a", "tag", passwordType, make([]byte, 50))
	_, err := db.InsertSyncEntity(ctx, first)
	require.NoError(t, err)
	deleteEntity(t, db, first, true)

	_, err = db.InsertSyncEntity(ctx, tagged("b", "tag", passwordType, make([]byte, 120)))
	require.ErrorIs(t, err, datastore.ErrQuotaExceeded)
	got := entityByID(t, db, "a")
	require.Len(t, got, 1)
	assert.True(t, *got[0].Deleted, "the tombstone stays")

	// Within the quota: the tombstone's byte is replaced, not added to.
	_, err = db.InsertSyncEntity(ctx, tagged("b", "tag", passwordType, make([]byte, 100)))
	require.NoError(t, err)
	bytes, err := db.ChainBytes(ctx, "c1")
	require.NoError(t, err)
	assert.Equal(t, int64(100), bytes)
}

func TestSQLiteUndeleteByUpdateTakesTheTag(t *testing.T) {
	// A client that keeps its edit over a remote delete commits an update of
	// the tombstone with deleted = false: the tag is the entity's again.
	ctx := context.Background()
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	first := tagged("a", "tag", passwordType, []byte{1})
	_, err := db.InsertSyncEntity(ctx, first)
	require.NoError(t, err)
	deletedAt := deleteEntity(t, db, first, true)

	edit := tagged("a", "tag", passwordType, []byte{1, 2})
	conflict, deleted, err := db.UpdateSyncEntity(ctx, edit, deletedAt)
	require.NoError(t, err)
	assert.False(t, conflict)
	assert.False(t, deleted)
	tags, err := db.AllTagItems(ctx)
	require.NoError(t, err)
	require.Len(t, tags, 1)

	// The tag is live again: a create replaces the entity under its ID.
	create := tagged("b", "tag", passwordType, []byte{3})
	conflict, err = db.InsertSyncEntity(ctx, create)
	require.NoError(t, err)
	assert.False(t, conflict)
	assert.Equal(t, "a", create.ID)
	got := entityByID(t, db, "a")
	require.Len(t, got, 1)
	assert.Equal(t, []byte{3}, got[0].Specifics)
	tags, err = db.AllTagItems(ctx)
	require.NoError(t, err)
	assert.Len(t, tags, 1)
}
