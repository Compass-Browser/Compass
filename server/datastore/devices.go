package datastore

import (
	"context"
	"crypto/sha1" //nolint:gosec // G505: Chromium's client tag hash is SHA-1; not used for security.
	"database/sql"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"
	"time"

	"google.golang.org/protobuf/encoding/protowire"
)

// Compass: removing a device from a chain (docs/design/sync.md, "Identity",
// "Removing a device"). A device is one installation's cache GUID in a
// chain. Removing it tombstones its DEVICE_INFO entity (command.
// DeleteDeviceInfo) and records the GUID here; requests carrying it as
// client_id are refused from then on (auth.RejectRemovedDevices), so a
// device still running doesn't write its entry back. Chain IDs are stored
// lower-case; cache GUIDs are compared exactly.

const removedDevicesSchema = `
CREATE TABLE IF NOT EXISTS removed_devices (
	client_id  TEXT NOT NULL,
	cache_guid TEXT NOT NULL,
	removed_at INTEGER NOT NULL,
	PRIMARY KEY (client_id, cache_guid)
) WITHOUT ROWID;
`

// DeviceInfoTypeID is DEVICE_INFO's EntitySpecifics field number.
const DeviceInfoTypeID = 154522

// ClientTagHash is Chromium's ClientTagHash::FromUnhashed: base64(SHA-1(the
// serialized EntitySpecifics with an empty field of |dataType| || |tag|)),
// the value clients send as client_tag_hash (stored as
// client_defined_unique_tag).
func ClientTagHash(dataType int, tag string) string {
	input := protowire.AppendTag(nil, protowire.Number(dataType), protowire.BytesType)
	input = protowire.AppendBytes(input, nil)
	input = append(input, tag...)
	sum := sha1.Sum(input) //nolint:gosec // G401: see the import.
	return base64.StdEncoding.EncodeToString(sum[:])
}

// DeviceInfoTag is the client tag of a device's DEVICE_INFO entity.
func DeviceInfoTag(cacheGUID string) string { return "DeviceInfo_" + cacheGUID }

// RemovedDevice is one removal.
type RemovedDevice struct {
	CacheGUID string
	RemovedAt time.Time
}

// RemoveDevice records that |cacheGUID| was removed from chain |clientID|
// (any case) at |now|, and deletes its push registrations. Removing it
// again keeps the first time.
func (s *SQLite) RemoveDevice(ctx context.Context, clientID, cacheGUID string, now time.Time) error {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	if _, err = tx.ExecContext(ctx, `
		INSERT INTO removed_devices (client_id, cache_guid, removed_at) VALUES (?, ?, ?)
		ON CONFLICT DO NOTHING`,
		strings.ToLower(clientID), cacheGUID, now.Unix()); err != nil {
		return fmt.Errorf("error recording removed device: %w", err)
	}
	if _, err = tx.ExecContext(ctx,
		`DELETE FROM push_tokens WHERE lower(client_id) = lower(?) AND cache_guid = ?`,
		clientID, cacheGUID); err != nil {
		return fmt.Errorf("error deleting the removed device's push tokens: %w", err)
	}
	if err = tx.Commit(); err != nil {
		return fmt.Errorf("error recording removed device: %w", err)
	}
	return nil
}

// IsDeviceRemoved says whether |cacheGUID| was removed from chain
// |clientID| (any case).
func (s *SQLite) IsDeviceRemoved(ctx context.Context, clientID, cacheGUID string) (bool, error) {
	return s.exists(ctx, `SELECT 1 FROM removed_devices WHERE client_id = ? AND cache_guid = ?`,
		strings.ToLower(clientID), cacheGUID)
}

// RemovedDevices returns the devices removed from chain |clientID| (any
// case), by cache GUID.
func (s *SQLite) RemovedDevices(ctx context.Context, clientID string) ([]RemovedDevice, error) {
	rows, err := s.r.QueryContext(ctx, `
		SELECT cache_guid, removed_at FROM removed_devices
		WHERE client_id = ? ORDER BY cache_guid`, strings.ToLower(clientID))
	if err != nil {
		return nil, fmt.Errorf("error reading removed devices: %w", err)
	}
	defer rows.Close()
	var out []RemovedDevice
	for rows.Next() {
		var d RemovedDevice
		var at int64
		if err = rows.Scan(&d.CacheGUID, &at); err != nil {
			return nil, fmt.Errorf("error reading removed devices: %w", err)
		}
		d.RemovedAt = time.Unix(at, 0)
		out = append(out, d)
	}
	if err = rows.Err(); err != nil {
		return nil, fmt.Errorf("error reading removed devices: %w", err)
	}
	return out, nil
}

// EntityByClientTag returns the latest entity (by mtime) of chain
// |clientID| (as stored) and |dataType| with client tag hash |tagHash|, or
// nil.
func (s *SQLite) EntityByClientTag(ctx context.Context, clientID string, dataType int, tagHash string) (*SyncEntity, error) {
	row := s.r.QueryRowContext(ctx, `SELECT `+entityColumns+` FROM entities
		WHERE client_id = ? AND client_defined_unique_tag = ? AND data_type = ?
		ORDER BY mtime DESC, rowid DESC LIMIT 1`, clientID, tagHash, dataType)
	e, err := scanEntity(row)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("error reading the entity of a client tag: %w", err)
	}
	return &e, nil
}

// EntitiesOfType returns every entity of |dataType| in chain |clientID| (as
// stored), deleted ones included, oldest first (admin listings).
func (s *SQLite) EntitiesOfType(ctx context.Context, clientID string, dataType int) ([]SyncEntity, error) {
	rows, err := s.r.QueryContext(ctx, `SELECT `+entityColumns+` FROM entities
		WHERE client_id = ? AND data_type = ? ORDER BY mtime, rowid`, clientID, dataType)
	if err != nil {
		return nil, fmt.Errorf("error reading entities: %w", err)
	}
	return scanEntities(rows)
}
