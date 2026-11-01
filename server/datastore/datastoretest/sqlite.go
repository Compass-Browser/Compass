package datastoretest

import (
	"context"
	"path/filepath"

	"github.com/brave/go-sync/datastore"
)

// NewSQLite opens a fresh SQLite datastore in dir (use t.TempDir()).
func NewSQLite(dir string) (*datastore.SQLite, error) {
	return datastore.NewSQLite(filepath.Join(dir, "test.db"), datastore.SQLiteOptions{})
}

// DeleteTable empties the datastore (go-sync dropped its DynamoDB table).
func DeleteTable(db *datastore.SQLite) error {
	return db.DeleteAll(context.Background())
}

// ResetTable empties the datastore.
func ResetTable(db *datastore.SQLite) error {
	return db.DeleteAll(context.Background())
}

// ScanSyncEntities returns all sync items.
func ScanSyncEntities(db *datastore.SQLite) ([]datastore.SyncEntity, error) {
	return db.AllSyncEntities(context.Background())
}

// ScanTagItems returns all tag items (and disabled-chain markers).
func ScanTagItems(db *datastore.SQLite) ([]datastore.ServerClientUniqueTagItem, error) {
	return db.AllTagItems(context.Background())
}

// ScanClientItemCounts returns all client item counts.
func ScanClientItemCounts(db *datastore.SQLite) ([]datastore.ClientItemCounts, error) {
	return db.AllItemCounts(context.Background())
}
