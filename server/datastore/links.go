package datastore

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

// Compass: linked chains (docs/design/sync.md, "Profiles"). Every profile of
// a sync chain but the home syncs in an account of its own, whose chain ID
// a private server's allowlist (COMPASS_SYNC_ALLOWED_CHAINS) can't know in
// advance. A chain that may use the server links such a chain
// (POST /v2/compass/account/link); the link records the root it came from,
// the listed chain, so a profile linking another profile still points at
// the home. Chain IDs are stored lower-case.

const chainLinksSchema = `
CREATE TABLE IF NOT EXISTS chain_links (
	client_id TEXT PRIMARY KEY,
	root      TEXT NOT NULL,
	created   INTEGER NOT NULL
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS chain_links_by_root ON chain_links (root);
`

// MaxLinksPerChain caps the chains linked from one root (profiles of one
// chain; a listed chain shouldn't open the server to everyone).
const MaxLinksPerChain = 64

// ErrTooManyLinks is LinkChainToRoot's error when the root has
// MaxLinksPerChain links already.
var ErrTooManyLinks = errors.New("too many linked chains")

// LinkChainToRoot links chain |to| from chain |from|: |to| gets |from|'s
// root (|from| itself if it isn't linked). Linking a chain to itself, or to
// the root it already has, does nothing.
func (s *SQLite) LinkChainToRoot(ctx context.Context, from, to string, now time.Time) error {
	from, to = strings.ToLower(from), strings.ToLower(to)
	root, err := s.ChainLinkRoot(ctx, from)
	if err != nil {
		return err
	}
	if root == "" {
		root = from
	}
	if to == root || to == from {
		return nil
	}
	if have, err := s.ChainLinkRoot(ctx, to); err != nil {
		return err
	} else if have == root {
		return nil
	}
	var n int
	if err := s.r.QueryRowContext(ctx, `SELECT COUNT(*) FROM chain_links WHERE root = ?`, root).
		Scan(&n); err != nil {
		return fmt.Errorf("error counting linked chains: %w", err)
	}
	if n >= MaxLinksPerChain {
		return ErrTooManyLinks
	}
	if _, err := s.w.ExecContext(ctx, `
		INSERT INTO chain_links (client_id, root, created) VALUES (?, ?, ?)
		ON CONFLICT (client_id) DO UPDATE SET root = excluded.root, created = excluded.created`,
		to, root, now.UnixMilli()); err != nil {
		return fmt.Errorf("error linking chain: %w", err)
	}
	return nil
}

// ChainLinkRoot returns the chain |clientID| was linked from (its root), ""
// if it isn't linked.
func (s *SQLite) ChainLinkRoot(ctx context.Context, clientID string) (string, error) {
	var root string
	err := s.r.QueryRowContext(ctx, `SELECT root FROM chain_links WHERE client_id = ?`,
		strings.ToLower(clientID)).Scan(&root)
	if errors.Is(err, sql.ErrNoRows) {
		return "", nil
	}
	if err != nil {
		return "", fmt.Errorf("error reading chain link: %w", err)
	}
	return root, nil
}
