package server_test

// Slice 7 (sync.md, "Slice 7 notes"): the quota status endpoint end to end,
// the new settings, backups and the hourly purge.

import (
	"context"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/quota"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
	"github.com/brave/go-sync/server"
)

func (d *device) quota(token string) (int, quota.Status) {
	d.t.Helper()
	req, err := http.NewRequest(http.MethodGet, d.url+"/v2/compass/quota", nil)
	require.NoError(d.t, err)
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	resp, err := http.DefaultClient.Do(req)
	require.NoError(d.t, err)
	defer resp.Body.Close()
	var st quota.Status
	if resp.StatusCode == http.StatusOK {
		require.NoError(d.t, json.NewDecoder(resp.Body).Decode(&st))
	}
	return resp.StatusCode, st
}

func TestEndToEndQuotaStatus(t *testing.T) {
	url := serverURL(t)
	a := newDevice(t, url, randomSeed(t), "cache-guid-a")

	code, _ := a.quota("")
	assert.Equal(t, http.StatusUnauthorized, code)

	initial := a.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID, bookmarkTypeID)
	bar := findByTag(initial, "bookmark_bar")
	require.NotNil(t, bar)
	a.commit(bookmark("local-1", bar.GetIdString(), "one", 0),
		&sync_pb.SyncEntity{IdString: ptr.String("h"), Version: ptr.Int64(0),
			ClientTagHash: ptr.String("h-tag"), Specifics: &sync_pb.EntitySpecifics{
				SpecificsVariant: &sync_pb.EntitySpecifics_History{History: &sync_pb.HistorySpecifics{}},
			}})

	code, st := a.quota(a.token(time.Now()))
	require.Equal(t, http.StatusOK, code)
	assert.Equal(t, 2, st.Entities) // The bookmark and the visit.
	assert.Equal(t, 1, st.History)
	assert.Equal(t, 100000, st.MaxEntities)
	assert.Equal(t, 30000, st.MaxHistory)
	assert.Equal(t, 14, st.HistoryDays)
	assert.Positive(t, st.Bytes)
	assert.Empty(t, st.Full)
}

func TestLoadConfigSlice7(t *testing.T) {
	t.Setenv("COMPASS_SYNC_DB", "/srv/sync/db.sqlite")
	cfg, err := server.LoadConfig()
	require.NoError(t, err)
	assert.Equal(t, 30, cfg.TombstoneDays)
	assert.Equal(t, 180, cfg.IdleChainDays)
	assert.Equal(t, server.BackupConfig{Dir: "/srv/sync/backups", Interval: 24 * time.Hour, Keep: 7}, cfg.Backup)

	t.Setenv("COMPASS_SYNC_TOMBSTONE_DAYS", "0")
	t.Setenv("COMPASS_SYNC_IDLE_CHAIN_DAYS", "365")
	t.Setenv("COMPASS_SYNC_BACKUP_DIR", "/backups")
	t.Setenv("COMPASS_SYNC_BACKUP_INTERVAL", "6h")
	t.Setenv("COMPASS_SYNC_BACKUP_KEEP", "3")
	cfg, err = server.LoadConfig()
	require.NoError(t, err)
	assert.Equal(t, 0, cfg.TombstoneDays)
	assert.Equal(t, 365, cfg.IdleChainDays)
	assert.Equal(t, server.BackupConfig{Dir: "/backups", Interval: 6 * time.Hour, Keep: 3}, cfg.Backup)

	t.Setenv("COMPASS_SYNC_BACKUP_INTERVAL", "0")
	cfg, err = server.LoadConfig()
	require.NoError(t, err)
	assert.Zero(t, cfg.Backup.Interval)

	for name, v := range map[string]string{
		"COMPASS_SYNC_TOMBSTONE_DAYS":  "-1",
		"COMPASS_SYNC_IDLE_CHAIN_DAYS": "soon",
		"COMPASS_SYNC_BACKUP_INTERVAL": "5s",
		"COMPASS_SYNC_BACKUP_KEEP":     "0",
	} {
		t.Run(name, func(t *testing.T) {
			t.Setenv(name, v)
			_, err := server.LoadConfig()
			assert.Error(t, err)
		})
	}
}

func TestBackupRotation(t *testing.T) {
	ctx := context.Background()
	dir := t.TempDir()
	db, err := datastore.NewSQLite(filepath.Join(dir, "compass-sync.db"), datastore.SQLiteOptions{})
	require.NoError(t, err)
	defer db.Close()
	_, err = db.InsertSyncEntity(ctx, &datastore.SyncEntity{
		ClientID: "c", ID: "e", Version: ptr.Int64(1), Mtime: ptr.Int64(1), Ctime: ptr.Int64(1),
		DataType: ptr.Int(123), Folder: ptr.Bool(false), Deleted: ptr.Bool(false), Specifics: []byte("x"),
	})
	require.NoError(t, err)

	cfg := server.BackupConfig{Dir: filepath.Join(dir, "backups"), Interval: time.Hour, Keep: 2}
	// No backup yet: one is due now.
	assert.Zero(t, server.NextBackupIn(cfg, time.Now()))

	t0 := time.Date(2026, 10, 31, 22, 0, 0, 0, time.UTC)
	var paths []string
	for i := range 3 {
		p, err := server.RunBackup(ctx, db, cfg, t0.Add(time.Duration(i)*time.Hour))
		require.NoError(t, err)
		paths = append(paths, p)
	}
	assert.Equal(t, filepath.Join(cfg.Dir, "compass-sync-20261031T220000Z.db"), paths[0])

	// The oldest went; no temporary file is left.
	entries, err := os.ReadDir(cfg.Dir)
	require.NoError(t, err)
	var names []string
	for _, e := range entries {
		names = append(names, e.Name())
	}
	assert.Equal(t, []string{"compass-sync-20261031T230000Z.db", "compass-sync-20261101T000000Z.db"}, names)

	// The next is due an interval after the newest.
	assert.Equal(t, 30*time.Minute, server.NextBackupIn(cfg, t0.Add(2*time.Hour+30*time.Minute)))
	assert.Zero(t, server.NextBackupIn(cfg, t0.Add(4*time.Hour)))

	// A backup is a database with the data.
	restored, err := datastore.NewSQLite(paths[2], datastore.SQLiteOptions{})
	require.NoError(t, err)
	defer restored.Close()
	has, err := restored.HasItem(ctx, "c", "e")
	require.NoError(t, err)
	assert.True(t, has)
}

func TestPurgeTombstonesAndIdleChains(t *testing.T) {
	ctx := context.Background()
	db, err := datastore.NewSQLite(filepath.Join(t.TempDir(), "p.db"), datastore.SQLiteOptions{})
	require.NoError(t, err)
	defer db.Close()
	now := time.Now()
	old := now.AddDate(0, 0, -45).UnixMilli()

	insert := func(chain, id string, deleted bool) {
		_, err := db.InsertSyncEntity(ctx, &datastore.SyncEntity{
			ClientID: chain, ID: id, Version: ptr.Int64(old), Mtime: ptr.Int64(old), Ctime: ptr.Int64(old),
			DataType: ptr.Int(123), Folder: ptr.Bool(false), Deleted: ptr.Bool(deleted), Specifics: []byte("x"),
		})
		require.NoError(t, err)
	}
	insert("live", "tomb", true)
	insert("live", "kept", false)
	insert("gone", "g", false)
	require.NoError(t, db.TouchChain(ctx, "live", now))
	require.NoError(t, db.TouchChain(ctx, "gone", now.AddDate(0, 0, -200)))

	cfg := server.Config{TombstoneDays: 30, IdleChainDays: 180}
	server.Purge(ctx, db, cfg, now)

	all, err := db.AllSyncEntities(ctx)
	require.NoError(t, err)
	require.Len(t, all, 1)
	assert.Equal(t, "kept", all[0].ID)
	disabled, err := db.IsSyncChainDisabled(ctx, "gone")
	require.NoError(t, err)
	assert.True(t, disabled)

	// 0 turns both off.
	insert("live", "tomb2", true)
	require.NoError(t, db.TouchChain(ctx, "other", now.AddDate(0, 0, -200)))
	server.Purge(ctx, db, server.Config{}, now)
	all, err = db.AllSyncEntities(ctx)
	require.NoError(t, err)
	assert.Len(t, all, 2)
	disabled, err = db.IsSyncChainDisabled(ctx, "other")
	require.NoError(t, err)
	assert.False(t, disabled)
}
