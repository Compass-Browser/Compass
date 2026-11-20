package datastore_test

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

// Chromium's ClientTagHashTest.ShouldGenerateFromUnhashed (PREFERENCES is
// field 37702, AUTOFILL 31729).
func TestClientTagHashMatchesChromium(t *testing.T) {
	assert.Equal(t, "iNFQtRFQb+IZcn1kKUJEZDDkLs4=", datastore.ClientTagHash(37702, "tag1"))
	assert.Equal(t, "gO1cPZQXaM73sHOvSA+tKCKFs58=", datastore.ClientTagHash(31729, "tag1"))
	assert.Equal(t, "XYxkF7bhS4eItStFgiOIAU23swI=", datastore.ClientTagHash(37702, "tag2"))
	assert.Equal(t, "GFiWzo5NGhjLlN+OyCfhy28DJTQ=", datastore.ClientTagHash(31729, "tag2"))
	assert.Equal(t, "DeviceInfo_abc", datastore.DeviceInfoTag("abc"))
}

func TestRemoveDevice(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "d.db"), 0)
	ctx := context.Background()
	now := time.Unix(1_800_000_000, 0)
	require.NoError(t, db.SetPushToken(ctx, "ABCD", datastore.PushToken{CacheGUID: "g1", Token: "aa",
		Environment: "sandbox", Topic: "t"}, now))
	require.NoError(t, db.SetPushToken(ctx, "ABCD", datastore.PushToken{CacheGUID: "g2", Token: "bb",
		Environment: "sandbox", Topic: "t"}, now))

	require.NoError(t, db.RemoveDevice(ctx, "ABCD", "g1", now))
	// Again, later: idempotent, keeps the first time.
	require.NoError(t, db.RemoveDevice(ctx, "abcd", "g1", now.Add(time.Hour)))

	for _, c := range []struct {
		chain, guid string
		want        bool
	}{
		{"ABCD", "g1", true},
		{"abcd", "g1", true},  // Chain IDs: any case.
		{"abcd", "G1", false}, // Cache GUIDs: exactly.
		{"abcd", "g2", false},
		{"other", "g1", false},
	} {
		got, err := db.IsDeviceRemoved(ctx, c.chain, c.guid)
		require.NoError(t, err)
		assert.Equal(t, c.want, got, "%s %s", c.chain, c.guid)
	}
	removed, err := db.RemovedDevices(ctx, "AbCd")
	require.NoError(t, err)
	require.Len(t, removed, 1)
	assert.Equal(t, "g1", removed[0].CacheGUID)
	assert.Equal(t, now.Unix(), removed[0].RemovedAt.Unix())

	// Its push registration went; the other device's stayed.
	toks, err := db.PushTokens(ctx, "ABCD")
	require.NoError(t, err)
	require.Len(t, toks, 1)
	assert.Equal(t, "g2", toks[0].CacheGUID)

	// Clearing the chain (reset sync, an idle chain's expiry, users revoke
	// --delete-data) clears its removals.
	_, err = db.ClearServerData(ctx, "ABCD")
	require.NoError(t, err)
	gone, err := db.IsDeviceRemoved(ctx, "abcd", "g1")
	require.NoError(t, err)
	assert.False(t, gone)
}

func TestRemovedDevicesExpireWithTheChain(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "d.db"), 0)
	ctx := context.Background()
	old := time.Now().AddDate(0, 0, -200)
	require.NoError(t, db.TouchChain(ctx, "IDLE", old))
	require.NoError(t, db.RemoveDevice(ctx, "IDLE", "g", old))
	require.NoError(t, db.RemoveDevice(ctx, "busy", "g", old))
	require.NoError(t, db.TouchChain(ctx, "busy", time.Now()))
	expired, err := db.ExpireIdleChains(ctx, time.Now().AddDate(0, 0, -180))
	require.NoError(t, err)
	assert.Equal(t, []string{"IDLE"}, expired)
	gone, err := db.IsDeviceRemoved(ctx, "idle", "g")
	require.NoError(t, err)
	assert.False(t, gone)
	gone, err = db.IsDeviceRemoved(ctx, "busy", "g")
	require.NoError(t, err)
	assert.True(t, gone)
}

func TestEntityByClientTag(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "d.db"), 0)
	ctx := context.Background()
	tag := datastore.ClientTagHash(datastore.DeviceInfoTypeID, datastore.DeviceInfoTag("g"))
	e := entityAt("ABCD", "id-1", 1000, []byte{1})
	e.DataType = ptr.Int(datastore.DeviceInfoTypeID)
	e.ClientDefinedUniqueTag = ptr.String(tag)
	_, err := db.InsertSyncEntity(ctx, e)
	require.NoError(t, err)

	got, err := db.EntityByClientTag(ctx, "ABCD", datastore.DeviceInfoTypeID, tag)
	require.NoError(t, err)
	require.NotNil(t, got)
	assert.Equal(t, "id-1", got.ID)
	got, err = db.EntityByClientTag(ctx, "ABCD", 123, tag)
	require.NoError(t, err)
	assert.Nil(t, got)
	got, err = db.EntityByClientTag(ctx, "ABCD", datastore.DeviceInfoTypeID, "other")
	require.NoError(t, err)
	assert.Nil(t, got)

	all, err := db.EntitiesOfType(ctx, "ABCD", datastore.DeviceInfoTypeID)
	require.NoError(t, err)
	assert.Equal(t, []string{"id-1"}, ids(all))
}

// A database made by schema 3 gets removed_devices and keeps its data.
func TestMigrateSchema3(t *testing.T) {
	path := filepath.Join(t.TempDir(), "v3.db")
	db := openSQLite(t, path, 0)
	ctx := context.Background()
	require.NoError(t, db.TouchChain(ctx, "old", time.Now()))
	require.NoError(t, db.Close())
	raw, err := sql.Open("sqlite", "file:"+path)
	require.NoError(t, err)
	_, err = raw.Exec(`DROP TABLE removed_devices; PRAGMA user_version = 3;`)
	require.NoError(t, err)
	require.NoError(t, raw.Close())

	db = openSQLite(t, path, 0)
	last, err := db.LastActive(ctx, "old")
	require.NoError(t, err)
	assert.WithinDuration(t, time.Now(), last, time.Minute)
	require.NoError(t, db.RemoveDevice(ctx, "old", "g", time.Now()))
	gone, err := db.IsDeviceRemoved(ctx, "old", "g")
	require.NoError(t, err)
	assert.True(t, gone)
	require.NoError(t, db.Close())

	raw, err = sql.Open("sqlite", "file:"+path)
	require.NoError(t, err)
	defer raw.Close()
	var version int
	require.NoError(t, raw.QueryRow(`PRAGMA user_version`).Scan(&version))
	assert.Equal(t, 5, version)
}
