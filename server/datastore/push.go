package datastore

import (
	"context"
	"fmt"
	"time"
)

// Compass: APNs device tokens per chain (docs/design/send-tab.md,
// "Delivery"). A device registers one token per chain it's in, keyed by its
// cache GUID in that chain.

// MaxPushTokensPerChain caps the registrations a chain keeps; registering
// past it drops the oldest.
const MaxPushTokensPerChain = 20

// PushToken is one device's APNs registration.
type PushToken struct {
	CacheGUID   string
	Token       string // Hex.
	Environment string // "sandbox" or "production".
	Topic       string // The app's bundle ID.
}

const pushSchema = `
CREATE TABLE IF NOT EXISTS push_tokens (
	client_id   TEXT NOT NULL,
	cache_guid  TEXT NOT NULL,
	token       TEXT NOT NULL,
	environment TEXT NOT NULL,
	topic       TEXT NOT NULL,
	updated     INTEGER NOT NULL,
	PRIMARY KEY (client_id, cache_guid)
) WITHOUT ROWID;
`

// SetPushToken stores |tok| for |clientID|. The same token under another
// cache GUID (the device rejoined the chain) is replaced.
func (s *SQLite) SetPushToken(ctx context.Context, clientID string, tok PushToken, now time.Time) error {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	if _, err = tx.ExecContext(ctx,
		`DELETE FROM push_tokens WHERE client_id = ? AND token = ? AND cache_guid <> ?`,
		clientID, tok.Token, tok.CacheGUID); err != nil {
		return fmt.Errorf("error replacing push token: %w", err)
	}
	if _, err = tx.ExecContext(ctx, `
		INSERT INTO push_tokens (client_id, cache_guid, token, environment, topic, updated)
		VALUES (?, ?, ?, ?, ?, ?)
		ON CONFLICT (client_id, cache_guid) DO UPDATE SET
			token = excluded.token,
			environment = excluded.environment,
			topic = excluded.topic,
			updated = excluded.updated`,
		clientID, tok.CacheGUID, tok.Token, tok.Environment, tok.Topic, now.Unix()); err != nil {
		return fmt.Errorf("error storing push token: %w", err)
	}
	if _, err = tx.ExecContext(ctx, `
		DELETE FROM push_tokens WHERE client_id = ? AND cache_guid NOT IN (
			SELECT cache_guid FROM push_tokens WHERE client_id = ?
			ORDER BY updated DESC, cache_guid LIMIT ?)`,
		clientID, clientID, MaxPushTokensPerChain); err != nil {
		return fmt.Errorf("error capping push tokens: %w", err)
	}
	if err = tx.Commit(); err != nil {
		return fmt.Errorf("error storing push token: %w", err)
	}
	return nil
}

// DeletePushToken removes the registration of |cacheGUID|, if any.
func (s *SQLite) DeletePushToken(ctx context.Context, clientID, cacheGUID string) error {
	if _, err := s.w.ExecContext(ctx,
		`DELETE FROM push_tokens WHERE client_id = ? AND cache_guid = ?`,
		clientID, cacheGUID); err != nil {
		return fmt.Errorf("error deleting push token: %w", err)
	}
	return nil
}

// DropPushToken removes |token| wherever |clientID| has it (APNs said it's
// no longer valid).
func (s *SQLite) DropPushToken(ctx context.Context, clientID, token string) error {
	if _, err := s.w.ExecContext(ctx,
		`DELETE FROM push_tokens WHERE client_id = ? AND token = ?`,
		clientID, token); err != nil {
		return fmt.Errorf("error dropping push token: %w", err)
	}
	return nil
}

// PushTokens returns |clientID|'s registrations, by cache GUID.
func (s *SQLite) PushTokens(ctx context.Context, clientID string) ([]PushToken, error) {
	rows, err := s.r.QueryContext(ctx, `
		SELECT cache_guid, token, environment, topic FROM push_tokens
		WHERE client_id = ? ORDER BY cache_guid`, clientID)
	if err != nil {
		return nil, fmt.Errorf("error reading push tokens: %w", err)
	}
	defer rows.Close()
	var out []PushToken
	for rows.Next() {
		var t PushToken
		if err = rows.Scan(&t.CacheGUID, &t.Token, &t.Environment, &t.Topic); err != nil {
			return nil, fmt.Errorf("error reading push tokens: %w", err)
		}
		out = append(out, t)
	}
	if err = rows.Err(); err != nil {
		return nil, fmt.Errorf("error reading push tokens: %w", err)
	}
	return out, nil
}
