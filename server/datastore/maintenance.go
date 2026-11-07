package datastore

// Compass (sync.md, "Server"): housekeeping on the SQLite
// store. Tombstones go after a while, chains no device has used for a long
// time are cleared and disabled, and each chain remembers which quota last
// refused one of its commits, so its devices can say so.

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"sync"
	"time"
)

// QuotaFull says which of a chain's limits refused its latest commits.
type QuotaFull int

const (
	// QuotaEntities: the chain has as many live entities as allowed
	// (COMPASS_SYNC_MAX_ENTITIES); new entities get OVER_QUOTA.
	QuotaEntities QuotaFull = 1 << iota
	// QuotaBytes: the chain stores as many bytes as allowed
	// (COMPASS_SYNC_MAX_BYTES); growing entries get OVER_QUOTA.
	QuotaBytes
	// QuotaHistory: the chain's history quota (COMPASS_SYNC_MAX_HISTORY) is
	// used up; new visits are dropped (they still answer SUCCESS, so the
	// other types keep syncing).
	QuotaHistory
)

// touchInterval: TouchChain writes a chain's last_active at most this often.
const touchInterval = time.Hour

// toucher remembers when each chain's last_active was last written, so a
// request doesn't cost a write each time.
type toucher struct {
	mu   sync.Mutex
	last map[string]time.Time
}

// TouchChain records that a device of the chain talked to the server at
// |now| (for ExpireIdleChains). Cheap when called often: it writes at most
// once an hour per chain.
func (s *SQLite) TouchChain(ctx context.Context, clientID string, now time.Time) error {
	s.touch.mu.Lock()
	if s.touch.last == nil {
		s.touch.last = map[string]time.Time{}
	}
	if last, ok := s.touch.last[clientID]; ok && now.Sub(last) < touchInterval {
		s.touch.mu.Unlock()
		return nil
	}
	s.touch.last[clientID] = now
	s.touch.mu.Unlock()

	_, err := s.w.ExecContext(ctx, `
		INSERT INTO chains (client_id, last_active) VALUES (?, ?)
		ON CONFLICT (client_id) DO UPDATE SET last_active = excluded.last_active`,
		clientID, now.Unix())
	if err != nil {
		s.touch.mu.Lock()
		delete(s.touch.last, clientID)
		s.touch.mu.Unlock()
		return fmt.Errorf("error touching chain: %w", err)
	}
	return nil
}

// LastActive returns when a device of the chain last talked to the server,
// as recorded by TouchChain (zero if never).
func (s *SQLite) LastActive(ctx context.Context, clientID string) (time.Time, error) {
	var v sql.NullInt64
	err := s.r.QueryRowContext(ctx,
		`SELECT last_active FROM chains WHERE client_id = ?`, clientID).Scan(&v)
	if errors.Is(err, sql.ErrNoRows) || (err == nil && !v.Valid) {
		return time.Time{}, nil
	}
	if err != nil {
		return time.Time{}, fmt.Errorf("error reading last_active: %w", err)
	}
	return time.Unix(v.Int64, 0), nil
}

// PurgeTombstones deletes deleted entities whose last change (mtime) is
// before |before|. A device that was away longer doesn't learn of those
// deletions; an update it then commits for such an entity re-creates it
// (UpdateSyncEntity). Returns the number of entities deleted.
func (s *SQLite) PurgeTombstones(ctx context.Context, before time.Time) (int64, error) {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return 0, fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit

	cutoff := before.UnixMilli()
	const old = `deleted = 1 AND mtime < ?`
	// Tombstones keep no tag item (a delete frees it), but their names and
	// positions may still count towards the chain's bytes.
	if _, err = tx.ExecContext(ctx, `
		UPDATE chains SET bytes = MAX(0, bytes - (
			SELECT COALESCE(SUM(size), 0) FROM entities e
			WHERE e.client_id = chains.client_id AND e.deleted = 1 AND e.mtime < ?))
		WHERE client_id IN (SELECT DISTINCT client_id FROM entities WHERE `+old+`)`,
		cutoff, cutoff); err != nil {
		return 0, fmt.Errorf("error updating chain sizes: %w", err)
	}
	res, err := tx.ExecContext(ctx, `DELETE FROM entities WHERE `+old, cutoff)
	if err != nil {
		return 0, fmt.Errorf("error deleting tombstones: %w", err)
	}
	if err = tx.Commit(); err != nil {
		return 0, fmt.Errorf("error committing tombstone purge: %w", err)
	}
	return rowsAffected(res), nil
}

// ExpireIdleChains clears and disables every chain whose devices haven't
// talked to the server since |before| (TouchChain), as a ClearServerData
// ("reset sync" in Brave) would. A device that comes back to such a chain is
// told it's disabled (DISABLED_BY_ADMIN) and stops syncing; its local data
// stays. Returns the chains expired.
func (s *SQLite) ExpireIdleChains(ctx context.Context, before time.Time) ([]string, error) {
	rows, err := s.r.QueryContext(ctx, `
		SELECT client_id FROM chains
		WHERE disabled_reason IS NULL AND last_active IS NOT NULL AND last_active < ?`,
		before.Unix())
	if err != nil {
		return nil, fmt.Errorf("error finding idle chains: %w", err)
	}
	var idle []string
	for rows.Next() {
		var id string
		if err = rows.Scan(&id); err != nil {
			rows.Close()
			return nil, fmt.Errorf("error reading idle chains: %w", err)
		}
		idle = append(idle, id)
	}
	rows.Close()
	if err = rows.Err(); err != nil {
		return nil, fmt.Errorf("error reading idle chains: %w", err)
	}
	expired := []string{}
	for _, id := range idle {
		if err = s.DisableSyncChain(ctx, id); err != nil {
			return expired, err
		}
		if _, err = s.ClearServerData(ctx, id); err != nil {
			return expired, err
		}
		s.touch.mu.Lock()
		delete(s.touch.last, id)
		s.touch.mu.Unlock()
		expired = append(expired, id)
	}
	return expired, nil
}

// RecordQuota updates which limits refuse the chain's commits: the bits in
// |full| are set (now), then the bits in |cleared| that aren't in |full|
// are cleared. Writes only when something changes.
func (s *SQLite) RecordQuota(ctx context.Context, clientID string, full, cleared QuotaFull, now time.Time) error {
	cleared &^= full
	if full == 0 && cleared == 0 {
		return nil
	}
	if full != 0 {
		if _, err := s.w.ExecContext(ctx, `
			INSERT INTO chains (client_id, quota_full, quota_full_time) VALUES (?, ?, ?)
			ON CONFLICT (client_id) DO UPDATE SET
				quota_full = (quota_full | excluded.quota_full) & ~?,
				quota_full_time = excluded.quota_full_time`,
			clientID, int(full), now.Unix(), int(cleared)); err != nil {
			return fmt.Errorf("error recording quota: %w", err)
		}
		return nil
	}
	if _, err := s.w.ExecContext(ctx, `
		UPDATE chains SET quota_full = quota_full & ~?
		WHERE client_id = ? AND quota_full & ? != 0`,
		int(cleared), clientID, int(cleared)); err != nil {
		return fmt.Errorf("error clearing quota: %w", err)
	}
	return nil
}

// QuotaState returns the limits that last refused the chain's commits and
// when (zero time if none ever did).
func (s *SQLite) QuotaState(ctx context.Context, clientID string) (QuotaFull, time.Time, error) {
	var full int
	var at sql.NullInt64
	err := s.r.QueryRowContext(ctx,
		`SELECT quota_full, quota_full_time FROM chains WHERE client_id = ?`, clientID).Scan(&full, &at)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, time.Time{}, nil
	}
	if err != nil {
		return 0, time.Time{}, fmt.Errorf("error reading quota state: %w", err)
	}
	var t time.Time
	if at.Valid {
		t = time.Unix(at.Int64, 0)
	}
	return QuotaFull(full), t, nil
}

// QuotaRecorder is implemented by stores that remember quota refusals
// (SQLite, and the instrumented wrapper around it).
type QuotaRecorder interface {
	RecordQuota(ctx context.Context, clientID string, full, cleared QuotaFull, now time.Time) error
}

// ChainToucher is implemented by stores that track chain activity.
type ChainToucher interface {
	TouchChain(ctx context.Context, clientID string, now time.Time) error
}

// RecordQuota forwards to the wrapped store if it remembers quotas.
func (_d DatastoreWithPrometheus) RecordQuota(
	ctx context.Context, clientID string, full, cleared QuotaFull, now time.Time,
) error {
	if r, ok := _d.base.(QuotaRecorder); ok {
		return r.RecordQuota(ctx, clientID, full, cleared, now)
	}
	return nil
}

// TouchChain forwards to the wrapped store if it tracks activity.
func (_d DatastoreWithPrometheus) TouchChain(ctx context.Context, clientID string, now time.Time) error {
	if t, ok := _d.base.(ChainToucher); ok {
		return t.TouchChain(ctx, clientID, now)
	}
	return nil
}
