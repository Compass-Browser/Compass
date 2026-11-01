package datastore

// Compass: the SQLite store that replaces go-sync's DynamoDB one (dynamo.go,
// and the DynamoDB methods of sync_entity.go and item_count.go). It keeps
// the Datastore semantics: entity versions, mtime-ordered updates per type
// (the progress marker token is the last mtime returned), soft deletes
// (tombstones), unique client and server tags, item counts for the quota,
// disabled chains. One file, opened by one process.

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"net/url"
	"strings"
	"time"

	"github.com/rs/zerolog/log"
	_ "modernc.org/sqlite" // Pure Go driver: static binary, no cgo.
)

// ErrQuotaExceeded is returned by inserts and updates that would take a
// chain's stored bytes over the store's MaxBytesPerChain.
var ErrQuotaExceeded = errors.New("chain storage quota exceeded")

// schemaVersion is stored in PRAGMA user_version. 2 (slice 7): chains got
// last_active, quota_full and quota_full_time.
const schemaVersion = 2

const schema = `
CREATE TABLE IF NOT EXISTS entities (
	client_id                 TEXT NOT NULL,
	id                        TEXT NOT NULL,
	parent_id                 TEXT,
	version                   INTEGER,
	mtime                     INTEGER,
	ctime                     INTEGER,
	name                      TEXT,
	non_unique_name           TEXT,
	server_defined_unique_tag TEXT,
	deleted                   INTEGER,
	originator_cache_guid     TEXT,
	originator_client_item_id TEXT,
	specifics                 BLOB,
	data_type                 INTEGER,
	folder                    INTEGER,
	client_defined_unique_tag TEXT,
	unique_position           BLOB,
	data_type_mtime           TEXT,
	expiration_time           INTEGER,
	size                      INTEGER NOT NULL DEFAULT 0,
	PRIMARY KEY (client_id, id)
);
CREATE INDEX IF NOT EXISTS entities_by_type_mtime
	ON entities (client_id, data_type, mtime);
CREATE INDEX IF NOT EXISTS entities_by_expiration
	ON entities (expiration_time) WHERE expiration_time > 0;
-- A create finds the entity a client tag belongs to (InsertSyncEntity).
CREATE INDEX IF NOT EXISTS entities_by_client_tag
	ON entities (client_id, client_defined_unique_tag)
	WHERE client_defined_unique_tag IS NOT NULL;

-- "Client#<client tag hash>" and "Server#<server tag>" per chain, so a tag
-- is used by one live entity at a time.
CREATE TABLE IF NOT EXISTS unique_tags (
	client_id TEXT NOT NULL,
	id        TEXT NOT NULL,
	mtime     INTEGER,
	ctime     INTEGER,
	PRIMARY KEY (client_id, id)
) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS item_counts (
	client_id               TEXT PRIMARY KEY,
	item_count              INTEGER NOT NULL DEFAULT 0,
	history_period1         INTEGER NOT NULL DEFAULT 0,
	history_period2         INTEGER NOT NULL DEFAULT 0,
	history_period3         INTEGER NOT NULL DEFAULT 0,
	history_period4         INTEGER NOT NULL DEFAULT 0,
	last_period_change_time INTEGER NOT NULL DEFAULT 0,
	version                 INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;

-- Per chain: bytes stored (for the size quota), the disabled marker (set
-- by ClearServerData, i.e. "reset sync", and by ExpireIdleChains), when a
-- device last talked to the server (Unix seconds, TouchChain), and which
-- quota last refused a commit (QuotaFull bits, Unix seconds).
CREATE TABLE IF NOT EXISTS chains (
	client_id       TEXT PRIMARY KEY,
	bytes           INTEGER NOT NULL DEFAULT 0,
	disabled_reason TEXT,
	disabled_mtime  INTEGER,
	disabled_ctime  INTEGER,
	last_active     INTEGER,
	quota_full      INTEGER NOT NULL DEFAULT 0,
	quota_full_time INTEGER
) WITHOUT ROWID;
`

// chainColumnsV2 are the chains columns added in schema 2, for databases
// made by schema 1 (CREATE TABLE IF NOT EXISTS leaves those alone).
var chainColumnsV2 = []struct{ name, def string }{
	{"last_active", "INTEGER"},
	{"quota_full", "INTEGER NOT NULL DEFAULT 0"},
	{"quota_full_time", "INTEGER"},
}

// tieFallbackSize: see GetUpdatesForType.
const tieFallbackSize = 100

// SQLiteOptions configures NewSQLite.
type SQLiteOptions struct {
	// MaxBytesPerChain caps the bytes a chain stores (specifics, positions,
	// names). 0 means no cap.
	MaxBytesPerChain int64
}

// SQLite is a Datastore in one SQLite file.
type SQLite struct {
	w        *sql.DB // One connection: every write, serialized.
	r        *sql.DB // Readers (WAL lets them run beside the writer).
	maxBytes int64
	touch    toucher // TouchChain's write throttle.
}

func sqliteDSN(path string, readOnly bool) string {
	q := url.Values{}
	q.Add("_pragma", "busy_timeout(10000)")
	q.Add("_pragma", "journal_mode(WAL)")
	q.Add("_pragma", "synchronous(NORMAL)")
	q.Add("_pragma", "foreign_keys(1)")
	if readOnly {
		q.Add("_pragma", "query_only(1)")
	} else {
		q.Set("_txlock", "immediate")
	}
	return "file:" + path + "?" + q.Encode()
}

// NewSQLite opens (creating if needed) the database at path.
func NewSQLite(path string, opts SQLiteOptions) (*SQLite, error) {
	w, err := sql.Open("sqlite", sqliteDSN(path, false))
	if err != nil {
		return nil, fmt.Errorf("error opening %s: %w", path, err)
	}
	w.SetMaxOpenConns(1)
	w.SetConnMaxLifetime(0)
	w.SetConnMaxIdleTime(0)
	if err = migrate(w); err != nil {
		w.Close()
		return nil, err
	}
	var maxMtime sql.NullInt64
	if err = w.QueryRow(`SELECT MAX(mtime) FROM entities`).Scan(&maxMtime); err != nil {
		w.Close()
		return nil, fmt.Errorf("error reading the latest mtime: %w", err)
	}
	observeMtime(maxMtime.Int64)
	r, err := sql.Open("sqlite", sqliteDSN(path, true))
	if err != nil {
		w.Close()
		return nil, fmt.Errorf("error opening %s for reading: %w", path, err)
	}
	r.SetMaxOpenConns(8)
	return &SQLite{w: w, r: r, maxBytes: opts.MaxBytesPerChain}, nil
}

func migrate(db *sql.DB) error {
	var version int
	if err := db.QueryRow("PRAGMA user_version").Scan(&version); err != nil {
		return fmt.Errorf("error reading schema version: %w", err)
	}
	if version > schemaVersion {
		return fmt.Errorf("database schema %d is newer than this server (%d)", version, schemaVersion)
	}
	if _, err := db.Exec(schema); err != nil {
		return fmt.Errorf("error creating schema: %w", err)
	}
	if err := addMissingChainColumns(db); err != nil {
		return err
	}
	if version < 2 {
		// Chains from before last_active get a full idle period from now.
		if _, err := db.Exec(`UPDATE chains SET last_active = ? WHERE last_active IS NULL`,
			time.Now().Unix()); err != nil {
			return fmt.Errorf("error setting last_active: %w", err)
		}
	}
	if _, err := db.Exec(fmt.Sprintf("PRAGMA user_version = %d", schemaVersion)); err != nil {
		return fmt.Errorf("error writing schema version: %w", err)
	}
	return nil
}

func addMissingChainColumns(db *sql.DB) error {
	rows, err := db.Query(`SELECT name FROM pragma_table_info('chains')`)
	if err != nil {
		return fmt.Errorf("error reading the chains columns: %w", err)
	}
	have := map[string]bool{}
	for rows.Next() {
		var name string
		if err = rows.Scan(&name); err != nil {
			rows.Close()
			return fmt.Errorf("error reading the chains columns: %w", err)
		}
		have[name] = true
	}
	rows.Close()
	for _, c := range chainColumnsV2 {
		if have[c.name] {
			continue
		}
		if _, err = db.Exec(`ALTER TABLE chains ADD COLUMN ` + c.name + ` ` + c.def); err != nil {
			return fmt.Errorf("error adding chains.%s: %w", c.name, err)
		}
	}
	return nil
}

// Close closes the database.
func (s *SQLite) Close() error {
	return errors.Join(s.r.Close(), s.w.Close())
}

// Ping checks the database answers.
func (s *SQLite) Ping(ctx context.Context) error {
	var one int
	return s.r.QueryRowContext(ctx, "SELECT 1").Scan(&one)
}

const entityColumns = `client_id, id, parent_id, version, mtime, ctime, name,
	non_unique_name, server_defined_unique_tag, deleted, originator_cache_guid,
	originator_client_item_id, specifics, data_type, folder,
	client_defined_unique_tag, unique_position, data_type_mtime, expiration_time`

// blob stores empty byte slices as NULL, so they read back as nil.
func blob(b []byte) any {
	if len(b) == 0 {
		return nil
	}
	return b
}

func entityArgs(e *SyncEntity) []any {
	return []any{e.ClientID, e.ID, e.ParentID, e.Version, e.Mtime, e.Ctime,
		e.Name, e.NonUniqueName, e.ServerDefinedUniqueTag, e.Deleted,
		e.OriginatorCacheGUID, e.OriginatorClientItemID, blob(e.Specifics),
		e.DataType, e.Folder, e.ClientDefinedUniqueTag, blob(e.UniquePosition),
		e.DataTypeMtime, e.ExpirationTime}
}

func strPtr(v sql.NullString) *string {
	if !v.Valid {
		return nil
	}
	return &v.String
}

func int64Ptr(v sql.NullInt64) *int64 {
	if !v.Valid {
		return nil
	}
	return &v.Int64
}

func boolPtr(v sql.NullBool) *bool {
	if !v.Valid {
		return nil
	}
	return &v.Bool
}

func scanEntity(row interface{ Scan(...any) error }) (SyncEntity, error) {
	var e SyncEntity
	var parentID, name, nonUniqueName, serverTag, cacheGUID, clientItemID,
		clientTag, dataTypeMtime sql.NullString
	var version, mtime, ctime, dataType, expiration sql.NullInt64
	var deleted, folder sql.NullBool
	err := row.Scan(&e.ClientID, &e.ID, &parentID, &version, &mtime, &ctime,
		&name, &nonUniqueName, &serverTag, &deleted, &cacheGUID, &clientItemID,
		&e.Specifics, &dataType, &folder, &clientTag, &e.UniquePosition,
		&dataTypeMtime, &expiration)
	if err != nil {
		return e, err
	}
	e.ParentID = strPtr(parentID)
	e.Version = int64Ptr(version)
	e.Mtime = int64Ptr(mtime)
	e.Ctime = int64Ptr(ctime)
	e.Name = strPtr(name)
	e.NonUniqueName = strPtr(nonUniqueName)
	e.ServerDefinedUniqueTag = strPtr(serverTag)
	e.Deleted = boolPtr(deleted)
	e.OriginatorCacheGUID = strPtr(cacheGUID)
	e.OriginatorClientItemID = strPtr(clientItemID)
	if dataType.Valid {
		dt := int(dataType.Int64)
		e.DataType = &dt
	}
	e.Folder = boolPtr(folder)
	e.ClientDefinedUniqueTag = strPtr(clientTag)
	e.DataTypeMtime = strPtr(dataTypeMtime)
	e.ExpirationTime = int64Ptr(expiration)
	return e, nil
}

func scanEntities(rows *sql.Rows) ([]SyncEntity, error) {
	defer rows.Close()
	entities := []SyncEntity{}
	for rows.Next() {
		e, err := scanEntity(rows)
		if err != nil {
			return nil, err
		}
		entities = append(entities, e)
	}
	return entities, rows.Err()
}

func strLen(s *string) int64 {
	if s == nil {
		return 0
	}
	return int64(len(*s))
}

// entitySize is what an entity counts towards the chain's byte quota.
func entitySize(specifics, uniquePosition []byte, name, nonUniqueName *string) int64 {
	return int64(len(specifics)+len(uniquePosition)) + strLen(name) + strLen(nonUniqueName)
}

// addBytes adds delta to the chain's stored bytes, failing with
// ErrQuotaExceeded if a growth takes it over the cap.
func (s *SQLite) addBytes(ctx context.Context, tx *sql.Tx, clientID string, delta int64) error {
	if delta > 0 && s.maxBytes > 0 {
		var current int64
		err := tx.QueryRowContext(ctx,
			`SELECT bytes FROM chains WHERE client_id = ?`, clientID).Scan(&current)
		if err != nil && !errors.Is(err, sql.ErrNoRows) {
			return fmt.Errorf("error reading chain size: %w", err)
		}
		if current+delta > s.maxBytes {
			return ErrQuotaExceeded
		}
	}
	if delta == 0 {
		return nil
	}
	_, err := tx.ExecContext(ctx, `
		INSERT INTO chains (client_id, bytes) VALUES (?, MAX(?, 0))
		ON CONFLICT (client_id) DO UPDATE SET bytes = MAX(bytes + ?, 0)`,
		clientID, delta, delta)
	if err != nil {
		return fmt.Errorf("error updating chain size: %w", err)
	}
	return nil
}

func rowsAffected(res sql.Result) int64 {
	n, err := res.RowsAffected()
	if err != nil {
		return 0
	}
	return n
}

func (s *SQLite) insertTag(ctx context.Context, tx *sql.Tx, clientID, tag string, isServer bool) (bool, error) {
	item := NewServerClientUniqueTagItem(clientID, tag, isServer)
	res, err := tx.ExecContext(ctx, `
		INSERT INTO unique_tags (client_id, id, mtime, ctime) VALUES (?, ?, ?, ?)
		ON CONFLICT DO NOTHING`, item.ClientID, item.ID, item.Mtime, item.Ctime)
	if err != nil {
		return false, fmt.Errorf("error inserting tag item: %w", err)
	}
	return rowsAffected(res) == 1, nil
}

func (s *SQLite) insertEntity(ctx context.Context, tx *sql.Tx, entity *SyncEntity) (bool, error) {
	args := append(entityArgs(entity),
		entitySize(entity.Specifics, entity.UniquePosition, entity.Name, entity.NonUniqueName))
	res, err := tx.ExecContext(ctx, `INSERT INTO entities (`+entityColumns+`, size)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT DO NOTHING`, args...)
	if err != nil {
		return false, fmt.Errorf("error inserting sync item: %w", err)
	}
	return rowsAffected(res) == 1, nil
}

// taggedEntity is the latest entity of a data type holding a client tag.
type taggedEntity struct {
	id      string
	deleted bool
	size    int64
}

// findTaggedEntity returns the latest entity (by mtime) of entity's chain and
// data type with its client tag, or nil. go-sync gave a create over a
// tombstone a new ID, so older data can hold several rows with one tag; the
// latest is the one clients know.
func findTaggedEntity(ctx context.Context, tx *sql.Tx, entity *SyncEntity) (*taggedEntity, error) {
	var t taggedEntity
	var deleted sql.NullBool
	err := tx.QueryRowContext(ctx, `
		SELECT id, deleted, size FROM entities
		WHERE client_id = ? AND client_defined_unique_tag = ? AND data_type = ?
		ORDER BY mtime DESC, rowid DESC LIMIT 1`,
		entity.ClientID, *entity.ClientDefinedUniqueTag, *entity.DataType).
		Scan(&t.id, &deleted, &t.size)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("error reading the entity of a client tag: %w", err)
	}
	t.deleted = deleted.Valid && deleted.Bool
	return &t, nil
}

// reviveEntity writes a create over the tombstone old: the entity keeps
// old's ID and takes everything else from the create (version, times,
// content, deleted = false), as Chromium's servers do (LoopbackServer gives
// a client-tagged entity an ID derived from its tag, so a create replaces the
// tombstone). entity.ID becomes old's ID, for the commit response.
func (s *SQLite) reviveEntity(ctx context.Context, tx *sql.Tx, entity *SyncEntity, old *taggedEntity) error {
	size := entitySize(entity.Specifics, entity.UniquePosition, entity.Name, entity.NonUniqueName)
	if err := s.addBytes(ctx, tx, entity.ClientID, size-old.size); err != nil {
		return err
	}
	_, err := tx.ExecContext(ctx, `
		UPDATE entities SET
			parent_id = ?, version = ?, mtime = ?, ctime = ?, name = ?,
			non_unique_name = ?, server_defined_unique_tag = ?, deleted = ?,
			originator_cache_guid = ?, originator_client_item_id = ?,
			specifics = ?, folder = ?, unique_position = ?, data_type_mtime = ?,
			expiration_time = ?, size = ?
		WHERE client_id = ? AND id = ?`,
		entity.ParentID, entity.Version, entity.Mtime, entity.Ctime, entity.Name,
		entity.NonUniqueName, entity.ServerDefinedUniqueTag, entity.Deleted,
		entity.OriginatorCacheGUID, entity.OriginatorClientItemID,
		blob(entity.Specifics), entity.Folder, blob(entity.UniquePosition), entity.DataTypeMtime,
		entity.ExpirationTime, size, entity.ClientID, old.id)
	if err != nil {
		return fmt.Errorf("error reviving sync entity: %w", err)
	}
	// The tag item is normally gone with the delete; one left over (a delete
	// committed without the tag) is this entity's anyway.
	if _, err = s.insertTag(ctx, tx, entity.ClientID, *entity.ClientDefinedUniqueTag, false); err != nil {
		return err
	}
	entity.ID = old.id
	return nil
}

// InsertSyncEntity inserts a new sync entity. If ClientDefinedUniqueTag is
// set (and the type isn't history, which doesn't use tag items), a tag item
// is written in the same transaction; a tag already in use is a conflict.
//
// Compass: a create whose client tag belongs to a deleted entity of the same
// type revives that entity under its ID (Chromium's semantics, see
// reviveEntity) and sets entity.ID to it; a live one is a conflict.
func (s *SQLite) InsertSyncEntity(ctx context.Context, entity *SyncEntity) (bool, error) {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return false, fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit

	withTag := entity.ClientDefinedUniqueTag != nil && *entity.DataType != HistoryTypeID
	if withTag {
		old, findErr := findTaggedEntity(ctx, tx, entity)
		if findErr != nil {
			return false, findErr
		}
		if old != nil && !old.deleted {
			return true, errors.New("error inserting sync item with client tag: tag in use")
		}
		if old != nil && (entity.Deleted == nil || !*entity.Deleted) {
			if err = s.reviveEntity(ctx, tx, entity, old); err != nil {
				return false, err
			}
			if err = tx.Commit(); err != nil {
				return false, fmt.Errorf("error committing revived sync item: %w", err)
			}
			return false, nil
		}
	}

	err = s.addBytes(ctx, tx, entity.ClientID,
		entitySize(entity.Specifics, entity.UniquePosition, entity.Name, entity.NonUniqueName))
	if err != nil {
		return false, err
	}

	if withTag {
		inserted, tagErr := s.insertTag(ctx, tx, entity.ClientID, *entity.ClientDefinedUniqueTag, false)
		if tagErr != nil {
			return false, tagErr
		}
		if !inserted {
			return true, errors.New("error inserting sync item with client tag: tag in use")
		}
	}

	inserted, err := s.insertEntity(ctx, tx, entity)
	if err != nil {
		return false, err
	}
	if !inserted {
		return withTag, errors.New("error inserting sync item: ID in use")
	}
	if err = tx.Commit(); err != nil {
		return false, fmt.Errorf("error committing sync item: %w", err)
	}
	return false, nil
}

// HasServerDefinedUniqueTag checks whether a server tag is in use for a
// client.
func (s *SQLite) HasServerDefinedUniqueTag(ctx context.Context, clientID string, tag string) (bool, error) {
	return s.exists(ctx, `SELECT 1 FROM unique_tags WHERE client_id = ? AND id = ?`,
		clientID, serverTagItemPrefix+tag)
}

// HasItem checks whether a client has an entity with this ID.
func (s *SQLite) HasItem(ctx context.Context, clientID string, id string) (bool, error) {
	return s.exists(ctx, `SELECT 1 FROM entities WHERE client_id = ? AND id = ?`, clientID, id)
}

func (s *SQLite) exists(ctx context.Context, query string, args ...any) (bool, error) {
	var one int
	err := s.r.QueryRowContext(ctx, query, args...).Scan(&one)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("error checking if item exists: %w", err)
	}
	return true, nil
}

// InsertSyncEntitiesWithServerTags inserts entities with server-defined
// unique tags, each with its tag item, all in one transaction.
func (s *SQLite) InsertSyncEntitiesWithServerTags(ctx context.Context, entities []*SyncEntity) error {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit

	for _, entity := range entities {
		inserted, err := s.insertTag(ctx, tx, entity.ClientID, *entity.ServerDefinedUniqueTag, true)
		if err != nil {
			return err
		}
		if !inserted {
			return fmt.Errorf("error inserting sync entities with server tags: tag %q in use",
				*entity.ServerDefinedUniqueTag)
		}
		inserted, err = s.insertEntity(ctx, tx, entity)
		if err != nil {
			return err
		}
		if !inserted {
			return fmt.Errorf("error inserting sync entities with server tags: ID %q in use", entity.ID)
		}
		// Server-defined entities are a handful of empty folders: no quota
		// check, but they count.
		size := entitySize(entity.Specifics, entity.UniquePosition, entity.Name, entity.NonUniqueName)
		if _, err = tx.ExecContext(ctx, `
			INSERT INTO chains (client_id, bytes) VALUES (?, ?)
			ON CONFLICT (client_id) DO UPDATE SET bytes = bytes + ?`,
			entity.ClientID, size, size); err != nil {
			return fmt.Errorf("error updating chain size: %w", err)
		}
	}
	if err = tx.Commit(); err != nil {
		return fmt.Errorf("error writing sync entities with server tags: %w", err)
	}
	return nil
}

// DisableSyncChain marks a chain as disabled so no further updates or
// commits can happen.
func (s *SQLite) DisableSyncChain(ctx context.Context, clientID string) error {
	now := time.Now().UnixMilli()
	_, err := s.w.ExecContext(ctx, `
		INSERT INTO chains (client_id, disabled_reason, disabled_mtime, disabled_ctime)
		VALUES (?, ?, ?, ?)
		ON CONFLICT (client_id) DO UPDATE SET
			disabled_reason = excluded.disabled_reason,
			disabled_mtime = excluded.disabled_mtime,
			disabled_ctime = excluded.disabled_ctime`,
		clientID, reasonDeleted, now, now)
	if err != nil {
		return fmt.Errorf("error disabling sync chain: %w", err)
	}
	return nil
}

// IsSyncChainDisabled checks whether a given sync chain has been deleted.
func (s *SQLite) IsSyncChainDisabled(ctx context.Context, clientID string) (bool, error) {
	return s.exists(ctx,
		`SELECT 1 FROM chains WHERE client_id = ? AND disabled_reason IS NOT NULL`, clientID)
}

// ClearServerData deletes all items for a given clientID (entities, tag
// items, item counts), keeping the disabled marker. It returns the deleted
// entities and tag items (key, Mtime, Version, DataType) so callers can
// invalidate caches.
func (s *SQLite) ClearServerData(ctx context.Context, clientID string) ([]SyncEntity, error) {
	syncEntities := []SyncEntity{}
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return syncEntities, fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit

	rows, err := tx.QueryContext(ctx, `
		SELECT client_id, id, mtime, version, data_type FROM entities WHERE client_id = ?
		UNION ALL
		SELECT client_id, id, mtime, NULL, NULL FROM unique_tags WHERE client_id = ?`,
		clientID, clientID)
	if err != nil {
		return syncEntities, fmt.Errorf("error querying sync entities to clear: %w", err)
	}
	for rows.Next() {
		var e SyncEntity
		var mtime, version, dataType sql.NullInt64
		if err = rows.Scan(&e.ClientID, &e.ID, &mtime, &version, &dataType); err != nil {
			rows.Close()
			return syncEntities, fmt.Errorf("error reading sync entities to clear: %w", err)
		}
		e.Mtime = int64Ptr(mtime)
		e.Version = int64Ptr(version)
		if dataType.Valid {
			dt := int(dataType.Int64)
			e.DataType = &dt
		}
		syncEntities = append(syncEntities, e)
	}
	rows.Close()
	if err = rows.Err(); err != nil {
		return syncEntities, fmt.Errorf("error reading sync entities to clear: %w", err)
	}

	for _, stmt := range []string{
		`DELETE FROM entities WHERE client_id = ?`,
		`DELETE FROM unique_tags WHERE client_id = ?`,
		`DELETE FROM item_counts WHERE client_id = ?`,
		`UPDATE chains SET bytes = 0 WHERE client_id = ?`,
	} {
		if _, err = tx.ExecContext(ctx, stmt, clientID); err != nil {
			return syncEntities, fmt.Errorf("error deleting sync entities for client %s: %w", clientID, err)
		}
	}
	if err = tx.Commit(); err != nil {
		return syncEntities, fmt.Errorf("error deleting sync entities for client %s: %w", clientID, err)
	}
	log.Info().Str("chainID", clientID).Int("count", len(syncEntities)).Msg("Cleared sync entities")
	return syncEntities, nil
}

// UpdateSyncEntity updates an existing sync entity if its version is
// oldVersion (any version for history). Optional fields are only written
// when set. Soft-deleting an entity with a client tag frees the tag.
func (s *SQLite) UpdateSyncEntity(ctx context.Context, entity *SyncEntity, oldVersion int64) (bool, bool, error) {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return false, false, fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit

	var version, oldSize sql.NullInt64
	var oldDeleted sql.NullBool
	var oldUniquePosition []byte
	var oldName, oldNonUniqueName sql.NullString
	err = tx.QueryRowContext(ctx, `
		SELECT version, deleted, size, unique_position, name, non_unique_name
		FROM entities WHERE client_id = ? AND id = ?`, entity.ClientID, entity.ID).
		Scan(&version, &oldDeleted, &oldSize, &oldUniquePosition, &oldName, &oldNonUniqueName)
	if errors.Is(err, sql.ErrNoRows) {
		// Compass (slice 7): the entity may have been a tombstone that
		// PurgeTombstones removed, or a history delete directive that
		// expired. Deleting it again is done already; an update from a
		// device that was away for that long re-creates it (as Chromium's
		// conflict resolution would: the local edit wins over the remote
		// delete), if it has a client tag to be found by. The new entity
		// doesn't count towards the entity quota until the next recount.
		if entity.Deleted != nil && *entity.Deleted {
			return false, false, nil
		}
		if entity.ClientDefinedUniqueTag != nil && *entity.DataType != HistoryTypeID {
			tx.Rollback() //nolint:errcheck // Released for InsertSyncEntity.
			if entity.Ctime == nil {
				entity.Ctime = entity.Mtime
			}
			if entity.Deleted == nil {
				entity.Deleted = new(bool)
			}
			if entity.Folder == nil {
				entity.Folder = new(bool)
			}
			conflict, insertErr := s.InsertSyncEntity(ctx, entity)
			return conflict, false, insertErr
		}
		return true, false, nil // Update only: a missing item is a conflict.
	}
	if err != nil {
		return false, false, fmt.Errorf("error reading sync entity to update: %w", err)
	}
	if *entity.DataType != HistoryTypeID && (!version.Valid || version.Int64 != oldVersion) {
		return true, false, nil
	}

	uniquePosition, name, nonUniqueName := entity.UniquePosition, entity.Name, entity.NonUniqueName
	if uniquePosition == nil {
		uniquePosition = oldUniquePosition
	}
	if name == nil {
		name = strPtr(oldName)
	}
	if nonUniqueName == nil {
		nonUniqueName = strPtr(oldNonUniqueName)
	}
	newSize := entitySize(entity.Specifics, uniquePosition, name, nonUniqueName)
	if err = s.addBytes(ctx, tx, entity.ClientID, newSize-oldSize.Int64); err != nil {
		return false, false, err
	}

	_, err = tx.ExecContext(ctx, `
		UPDATE entities SET
			version = ?, mtime = ?, specifics = ?, data_type_mtime = ?, size = ?,
			unique_position = COALESCE(?, unique_position),
			parent_id = COALESCE(?, parent_id),
			name = COALESCE(?, name),
			non_unique_name = COALESCE(?, non_unique_name),
			deleted = COALESCE(?, deleted),
			folder = COALESCE(?, folder)
		WHERE client_id = ? AND id = ?`,
		entity.Version, entity.Mtime, blob(entity.Specifics), entity.DataTypeMtime, newSize,
		blob(entity.UniquePosition), entity.ParentID, entity.Name, entity.NonUniqueName,
		entity.Deleted, entity.Folder, entity.ClientID, entity.ID)
	if err != nil {
		return false, false, fmt.Errorf("error updating sync entity: %w", err)
	}

	var deleted bool
	if entity.Deleted != nil && entity.ClientDefinedUniqueTag != nil && *entity.Deleted &&
		*entity.DataType != HistoryTypeID {
		// Soft-delete of an item with a client tag: free the tag.
		_, err = tx.ExecContext(ctx, `DELETE FROM unique_tags WHERE client_id = ? AND id = ?`,
			entity.ClientID, clientTagItemPrefix+*entity.ClientDefinedUniqueTag)
		if err != nil {
			return false, false, fmt.Errorf("error deleting tag item: %w", err)
		}
		deleted = true
	} else {
		if oldDeleted.Valid && oldDeleted.Bool && entity.Deleted != nil && !*entity.Deleted &&
			entity.ClientDefinedUniqueTag != nil && *entity.DataType != HistoryTypeID {
			// Compass: an update undeleting an entity with a client tag (a
			// client that kept its edit over a remote delete) takes the tag
			// back, so a later create with it conflicts.
			if _, err = s.insertTag(ctx, tx, entity.ClientID, *entity.ClientDefinedUniqueTag, false); err != nil {
				return false, false, err
			}
		}
		switch {
		case entity.Deleted == nil: // No updates on Deleted this time.
			deleted = false
		case !oldDeleted.Valid: // Consider it as Deleted = false.
			deleted = *entity.Deleted
		default:
			deleted = !oldDeleted.Bool && *entity.Deleted
		}
	}

	if err = tx.Commit(); err != nil {
		return false, false, fmt.Errorf("error committing sync entity update: %w", err)
	}
	return false, deleted, nil
}

// GetUpdatesForType returns up to maxSize sync entities of a data type whose
// mtime is later than clientToken, oldest first, skipping expired ones, and
// whether more may follow (true when the page is full, as go-sync did).
//
// The caller uses the last entity's mtime as the next token, and mtimes are
// milliseconds, so a page must not end inside a run of equal mtimes: the
// rest of the run would be skipped by the next request. Such a page is cut
// before the run. A page that is one run gets returned empty (with more to
// come) when small, as the caller then has room for it on the next request,
// or whole when it is tieFallbackSize or more, which needs that many
// commits in one millisecond.
func (s *SQLite) GetUpdatesForType(
	ctx context.Context,
	dataType int,
	clientToken int64,
	fetchFolders bool,
	clientID string,
	maxSize int,
) (bool, []SyncEntity, error) {
	if maxSize <= 0 {
		return true, []SyncEntity{}, nil
	}
	query := `SELECT ` + entityColumns + ` FROM entities
		WHERE client_id = ? AND data_type = ? AND mtime > ?
			AND (expiration_time IS NULL OR expiration_time <= 0 OR expiration_time >= ?)`
	if !fetchFolders {
		query += ` AND folder = 0`
	}
	query += ` ORDER BY mtime, rowid LIMIT ?`
	rows, err := s.r.QueryContext(ctx, query,
		clientID, dataType, clientToken, time.Now().Unix(), maxSize+1)
	if err != nil {
		return false, nil, fmt.Errorf("error doing query to get updates: %w", err)
	}
	entities, err := scanEntities(rows)
	if err != nil {
		return false, nil, fmt.Errorf("error reading updates: %w", err)
	}

	hasChangesRemaining := len(entities) >= maxSize
	if len(entities) > maxSize {
		next := *entities[maxSize].Mtime
		entities = entities[:maxSize]
		k := len(entities)
		for k > 0 && *entities[k-1].Mtime == next {
			k--
		}
		switch {
		case k > 0:
			entities = entities[:k]
		case maxSize < tieFallbackSize:
			entities = entities[:0]
		}
	}
	return hasChangesRemaining, entities, nil
}

// PurgeExpired deletes entities whose ExpirationTime (Unix seconds) is
// before now, as DynamoDB's TTL did for go-sync (history and history delete
// directives, after 14 days). Returns the number of entities deleted.
func (s *SQLite) PurgeExpired(ctx context.Context, now time.Time) (int64, error) {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return 0, fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit

	cutoff := now.Unix()
	const expired = `expiration_time > 0 AND expiration_time < ?`
	if _, err = tx.ExecContext(ctx, `
		DELETE FROM unique_tags WHERE (client_id, id) IN (
			SELECT client_id, ? || client_defined_unique_tag FROM entities
			WHERE `+expired+` AND client_defined_unique_tag IS NOT NULL AND data_type != ?)`,
		clientTagItemPrefix, cutoff, HistoryTypeID); err != nil {
		return 0, fmt.Errorf("error deleting expired tag items: %w", err)
	}
	if _, err = tx.ExecContext(ctx, `
		UPDATE chains SET bytes = MAX(0, bytes - (
			SELECT COALESCE(SUM(size), 0) FROM entities e
			WHERE e.client_id = chains.client_id AND `+strings.ReplaceAll(expired, "expiration_time", "e.expiration_time")+`))
		WHERE client_id IN (SELECT DISTINCT client_id FROM entities WHERE `+expired+`)`,
		cutoff, cutoff); err != nil {
		return 0, fmt.Errorf("error updating chain sizes: %w", err)
	}
	res, err := tx.ExecContext(ctx, `DELETE FROM entities WHERE `+expired, cutoff)
	if err != nil {
		return 0, fmt.Errorf("error deleting expired entities: %w", err)
	}
	if err = tx.Commit(); err != nil {
		return 0, fmt.Errorf("error committing purge: %w", err)
	}
	return rowsAffected(res), nil
}

// BackupTo writes a consistent, compacted copy of the database to path
// (VACUUM INTO), safe while the server runs. path must not exist.
func (s *SQLite) BackupTo(ctx context.Context, path string) error {
	if _, err := s.w.ExecContext(ctx, `VACUUM INTO ?`, path); err != nil {
		return fmt.Errorf("error backing up to %s: %w", path, err)
	}
	return nil
}

// ChainBytes returns the bytes a chain stores, as counted for the quota.
func (s *SQLite) ChainBytes(ctx context.Context, clientID string) (int64, error) {
	var n int64
	err := s.r.QueryRowContext(ctx, `SELECT bytes FROM chains WHERE client_id = ?`, clientID).Scan(&n)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, nil
	}
	return n, err
}

// AllSyncEntities returns every entity, ordered by client and ID (tests,
// debugging).
func (s *SQLite) AllSyncEntities(ctx context.Context) ([]SyncEntity, error) {
	rows, err := s.r.QueryContext(ctx,
		`SELECT `+entityColumns+` FROM entities ORDER BY client_id, id`)
	if err != nil {
		return nil, err
	}
	return scanEntities(rows)
}

// AllTagItems returns every tag item and disabled-chain marker, the rows of
// go-sync's table that had no Version (tests, debugging).
func (s *SQLite) AllTagItems(ctx context.Context) ([]ServerClientUniqueTagItem, error) {
	rows, err := s.r.QueryContext(ctx, `
		SELECT client_id, id, mtime, ctime FROM unique_tags
		UNION ALL
		SELECT client_id, ?, disabled_mtime, disabled_ctime FROM chains
		WHERE disabled_reason IS NOT NULL
		ORDER BY 1, 2`, disabledChainID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	items := []ServerClientUniqueTagItem{}
	for rows.Next() {
		var item ServerClientUniqueTagItem
		var mtime, ctime sql.NullInt64
		if err = rows.Scan(&item.ClientID, &item.ID, &mtime, &ctime); err != nil {
			return nil, err
		}
		item.Mtime = int64Ptr(mtime)
		item.Ctime = int64Ptr(ctime)
		items = append(items, item)
	}
	return items, rows.Err()
}

// AllItemCounts returns every item count row (tests, debugging).
func (s *SQLite) AllItemCounts(ctx context.Context) ([]ClientItemCounts, error) {
	rows, err := s.r.QueryContext(ctx, `SELECT `+itemCountColumns+` FROM item_counts ORDER BY client_id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	counts := []ClientItemCounts{}
	for rows.Next() {
		c, err := scanItemCounts(rows)
		if err != nil {
			return nil, err
		}
		counts = append(counts, c)
	}
	return counts, rows.Err()
}

// DeleteAll empties every table (tests).
func (s *SQLite) DeleteAll(ctx context.Context) error {
	_, err := s.w.ExecContext(ctx, `
		DELETE FROM entities; DELETE FROM unique_tags;
		DELETE FROM item_counts; DELETE FROM chains;`)
	return err
}
