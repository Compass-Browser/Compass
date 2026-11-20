package datastore

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"time"
)

// Compass: optional company sign-in (docs/design/sync.md, "Company sign-in
// (OIDC)"). An OIDC user (issuer + subject) signs in once per chain; the
// chain is then bound to the user. Chain IDs are stored lower-case (tokens
// carry the hex key in either case). A user's refresh token, from its
// latest sign-in, is what the server revalidates the user with; a binding
// made without one expires (session_expires, Unix ms; 0 = never).

const oidcSchema = `
CREATE TABLE IF NOT EXISTS oidc_users (
	id            INTEGER PRIMARY KEY,
	issuer        TEXT NOT NULL,
	subject       TEXT NOT NULL,
	email         TEXT NOT NULL DEFAULT '',
	name          TEXT NOT NULL DEFAULT '',
	created       INTEGER NOT NULL,
	last_login    INTEGER NOT NULL,
	revoked       INTEGER NOT NULL DEFAULT 0,
	refresh_token TEXT,
	last_checked  INTEGER,
	UNIQUE (issuer, subject)
);
CREATE TABLE IF NOT EXISTS chain_logins (
	client_id       TEXT PRIMARY KEY,
	user_id         INTEGER NOT NULL REFERENCES oidc_users (id) ON DELETE CASCADE,
	created         INTEGER NOT NULL,
	session_expires INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS chain_logins_by_user ON chain_logins (user_id);
`

// ErrOIDCUserRevoked is returned by SaveOIDCLogin for a user an admin
// revoked (users revoke), until restored.
var ErrOIDCUserRevoked = errors.New("user access revoked")

// OIDCUser is one signed-in identity.
type OIDCUser struct {
	ID           int64
	Issuer       string
	Subject      string
	Email        string
	Name         string
	Created      time.Time
	LastLogin    time.Time
	Revoked      bool
	RefreshToken string    // Empty: none.
	LastChecked  time.Time // Last successful revalidation (zero: never).
	Chains       int       // Bound chains (ListOIDCUsers only).
}

// ChainBinding is a chain's sign-in.
type ChainBinding struct {
	ClientID       string    // Lower-case.
	Created        time.Time // When it was bound.
	SessionExpires time.Time // Zero: no expiry (revalidated instead).
	LastActive     time.Time // From chains (ListChainBindings only).
	User           OIDCUser
}

// Valid says whether the binding lets the chain sync at |now|, for users of
// |issuer|.
func (b *ChainBinding) Valid(issuer string, now time.Time) bool {
	return b != nil && !b.User.Revoked && b.User.Issuer == issuer &&
		(b.SessionExpires.IsZero() || now.Before(b.SessionExpires))
}

// OIDCLogin is a completed sign-in, for SaveOIDCLogin.
type OIDCLogin struct {
	Issuer       string
	Subject      string
	Email        string
	Name         string
	RefreshToken string // Empty: keep the user's current one.
}

func unixMs(v sql.NullInt64) time.Time {
	if !v.Valid || v.Int64 == 0 {
		return time.Time{}
	}
	return time.UnixMilli(v.Int64)
}

func msOrZero(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.UnixMilli()
}

const oidcUserColumns = `u.id, u.issuer, u.subject, u.email, u.name, u.created,
	u.last_login, u.revoked, u.refresh_token, u.last_checked`

type rowScanner interface{ Scan(dest ...any) error }

func scanOIDCUser(row rowScanner, extra ...any) (OIDCUser, error) {
	var u OIDCUser
	var created, lastLogin, lastChecked sql.NullInt64
	var refresh sql.NullString
	dest := append([]any{&u.ID, &u.Issuer, &u.Subject, &u.Email, &u.Name, &created,
		&lastLogin, &u.Revoked, &refresh, &lastChecked}, extra...)
	if err := row.Scan(dest...); err != nil {
		return u, err
	}
	u.Created, u.LastLogin, u.LastChecked = unixMs(created), unixMs(lastLogin), unixMs(lastChecked)
	u.RefreshToken = refresh.String
	return u, nil
}

// SaveOIDCLogin records a sign-in of |login| and binds chain |clientID| to
// the user, until |sessionExpires| (zero: no expiry). Returns the user, or
// ErrOIDCUserRevoked (nothing bound).
func (s *SQLite) SaveOIDCLogin(ctx context.Context, login OIDCLogin, clientID string,
	sessionExpires, now time.Time) (OIDCUser, error) {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return OIDCUser{}, fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	nowMs := now.UnixMilli()
	var refresh, checked any
	if login.RefreshToken != "" {
		refresh, checked = login.RefreshToken, nowMs
	}
	if _, err = tx.ExecContext(ctx, `
		INSERT INTO oidc_users (issuer, subject, email, name, created, last_login, refresh_token, last_checked)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?)
		ON CONFLICT (issuer, subject) DO UPDATE SET
			email = excluded.email,
			name = excluded.name,
			last_login = CASE WHEN revoked THEN last_login ELSE excluded.last_login END,
			refresh_token = CASE WHEN revoked THEN refresh_token
				ELSE COALESCE(excluded.refresh_token, refresh_token) END,
			last_checked = CASE WHEN revoked THEN last_checked
				ELSE COALESCE(excluded.last_checked, last_checked) END`,
		login.Issuer, login.Subject, login.Email, login.Name, nowMs, nowMs, refresh, checked); err != nil {
		return OIDCUser{}, fmt.Errorf("error saving user: %w", err)
	}
	u, err := scanOIDCUser(tx.QueryRowContext(ctx,
		`SELECT `+oidcUserColumns+` FROM oidc_users u WHERE issuer = ? AND subject = ?`,
		login.Issuer, login.Subject))
	if err != nil {
		return u, fmt.Errorf("error reading user: %w", err)
	}
	if u.Revoked {
		return u, ErrOIDCUserRevoked
	}
	if _, err = tx.ExecContext(ctx, `
		INSERT INTO chain_logins (client_id, user_id, created, session_expires) VALUES (?, ?, ?, ?)
		ON CONFLICT (client_id) DO UPDATE SET
			user_id = excluded.user_id,
			created = excluded.created,
			session_expires = excluded.session_expires`,
		strings.ToLower(clientID), u.ID, nowMs, msOrZero(sessionExpires)); err != nil {
		return u, fmt.Errorf("error binding chain: %w", err)
	}
	if err = tx.Commit(); err != nil {
		return u, fmt.Errorf("error saving sign-in: %w", err)
	}
	return u, nil
}

// OIDCUserRevoked says whether the user (issuer, subject) exists and is
// revoked.
func (s *SQLite) OIDCUserRevoked(ctx context.Context, issuer, subject string) (bool, error) {
	return s.exists(ctx,
		`SELECT 1 FROM oidc_users WHERE issuer = ? AND subject = ? AND revoked`, issuer, subject)
}

// ChainBinding returns chain |clientID|'s binding, or nil if it has none.
func (s *SQLite) ChainBinding(ctx context.Context, clientID string) (*ChainBinding, error) {
	b := ChainBinding{ClientID: strings.ToLower(clientID)}
	var created, expires sql.NullInt64
	u, err := scanOIDCUser(s.r.QueryRowContext(ctx, `
		SELECT `+oidcUserColumns+`, c.created, c.session_expires
		FROM chain_logins c JOIN oidc_users u ON u.id = c.user_id
		WHERE c.client_id = ?`, b.ClientID), &created, &expires)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("error reading chain binding: %w", err)
	}
	b.User, b.Created, b.SessionExpires = u, unixMs(created), unixMs(expires)
	return &b, nil
}

// LinkChain binds chain |to| to the user |from| is bound to, with the same
// expiry. Returns false if |from| has no binding.
func (s *SQLite) LinkChain(ctx context.Context, from, to string, now time.Time) (bool, error) {
	res, err := s.w.ExecContext(ctx, `
		INSERT INTO chain_logins (client_id, user_id, created, session_expires)
		SELECT ?, user_id, ?, session_expires FROM chain_logins WHERE client_id = ?
		ON CONFLICT (client_id) DO UPDATE SET
			user_id = excluded.user_id,
			created = excluded.created,
			session_expires = excluded.session_expires`,
		strings.ToLower(to), now.UnixMilli(), strings.ToLower(from))
	if err != nil {
		return false, fmt.Errorf("error linking chain: %w", err)
	}
	return rowsAffected(res) > 0, nil
}

// UnbindChain removes chain |clientID|'s binding, if any.
func (s *SQLite) UnbindChain(ctx context.Context, clientID string) error {
	if _, err := s.w.ExecContext(ctx, `DELETE FROM chain_logins WHERE client_id = ?`,
		strings.ToLower(clientID)); err != nil {
		return fmt.Errorf("error unbinding chain: %w", err)
	}
	return nil
}

// OIDCUsersToRevalidate returns the users of |issuer| with a refresh token
// and a bound chain, not revoked, last revalidated before |before|.
func (s *SQLite) OIDCUsersToRevalidate(ctx context.Context, issuer string, before time.Time) ([]OIDCUser, error) {
	rows, err := s.r.QueryContext(ctx, `
		SELECT `+oidcUserColumns+` FROM oidc_users u
		WHERE u.issuer = ? AND NOT u.revoked AND u.refresh_token IS NOT NULL
			AND COALESCE(u.last_checked, 0) < ?
			AND EXISTS (SELECT 1 FROM chain_logins c WHERE c.user_id = u.id)
		ORDER BY u.last_checked`, issuer, before.UnixMilli())
	if err != nil {
		return nil, fmt.Errorf("error listing users to revalidate: %w", err)
	}
	return collectOIDCUsers(rows, false)
}

// collectOIDCUsers reads users, followed by their chain count if
// |withChains|.
func collectOIDCUsers(rows *sql.Rows, withChains bool) ([]OIDCUser, error) {
	defer rows.Close()
	var out []OIDCUser
	for rows.Next() {
		var chains int
		var extra []any
		if withChains {
			extra = []any{&chains}
		}
		u, err := scanOIDCUser(rows, extra...)
		u.Chains = chains
		if err != nil {
			return nil, fmt.Errorf("error reading users: %w", err)
		}
		out = append(out, u)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("error reading users: %w", err)
	}
	return out, nil
}

// UpdateOIDCUser records a successful revalidation of user |id| at |now|:
// a rotated refresh token (empty: unchanged) and the email and name from a
// refreshed ID token (empty: unchanged).
func (s *SQLite) UpdateOIDCUser(ctx context.Context, id int64, refreshToken, email, name string, now time.Time) error {
	if _, err := s.w.ExecContext(ctx, `
		UPDATE oidc_users SET
			refresh_token = COALESCE(NULLIF(?, ''), refresh_token),
			email = COALESCE(NULLIF(?, ''), email),
			name = COALESCE(NULLIF(?, ''), name),
			last_checked = ?
		WHERE id = ?`, refreshToken, email, name, now.UnixMilli(), id); err != nil {
		return fmt.Errorf("error updating user: %w", err)
	}
	return nil
}

// EndOIDCUserSessions unbinds every chain of user |id| and forgets its
// refresh token; with |revoke| also marks it revoked. Returns the chains
// unbound.
func (s *SQLite) EndOIDCUserSessions(ctx context.Context, id int64, revoke bool) ([]string, error) {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return nil, fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	rows, err := tx.QueryContext(ctx, `SELECT client_id FROM chain_logins WHERE user_id = ?`, id)
	if err != nil {
		return nil, fmt.Errorf("error listing user chains: %w", err)
	}
	chains, err := collectStrings(rows)
	if err != nil {
		return nil, err
	}
	if _, err = tx.ExecContext(ctx, `DELETE FROM chain_logins WHERE user_id = ?`, id); err != nil {
		return nil, fmt.Errorf("error unbinding user chains: %w", err)
	}
	if _, err = tx.ExecContext(ctx, `
		UPDATE oidc_users SET refresh_token = NULL, revoked = revoked OR ? WHERE id = ?`,
		revoke, id); err != nil {
		return nil, fmt.Errorf("error ending user sessions: %w", err)
	}
	if err = tx.Commit(); err != nil {
		return nil, fmt.Errorf("error ending user sessions: %w", err)
	}
	return chains, nil
}

// RestoreOIDCUser clears user |id|'s revoked mark: it can sign in again.
func (s *SQLite) RestoreOIDCUser(ctx context.Context, id int64) error {
	if _, err := s.w.ExecContext(ctx, `UPDATE oidc_users SET revoked = 0 WHERE id = ?`, id); err != nil {
		return fmt.Errorf("error restoring user: %w", err)
	}
	return nil
}

func collectStrings(rows *sql.Rows) ([]string, error) {
	defer rows.Close()
	var out []string
	for rows.Next() {
		var v string
		if err := rows.Scan(&v); err != nil {
			return nil, fmt.Errorf("error reading rows: %w", err)
		}
		out = append(out, v)
	}
	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("error reading rows: %w", err)
	}
	return out, nil
}

// ListOIDCUsers returns every user, with its number of bound chains.
func (s *SQLite) ListOIDCUsers(ctx context.Context) ([]OIDCUser, error) {
	rows, err := s.r.QueryContext(ctx, `
		SELECT `+oidcUserColumns+`,
			(SELECT COUNT(*) FROM chain_logins c WHERE c.user_id = u.id)
		FROM oidc_users u ORDER BY u.email, u.subject`)
	if err != nil {
		return nil, fmt.Errorf("error listing users: %w", err)
	}
	return collectOIDCUsers(rows, true)
}

// FindOIDCUsers returns the users whose email (case-insensitive) or
// subject is |emailOrSubject|.
func (s *SQLite) FindOIDCUsers(ctx context.Context, emailOrSubject string) ([]OIDCUser, error) {
	rows, err := s.r.QueryContext(ctx, `
		SELECT `+oidcUserColumns+` FROM oidc_users u
		WHERE lower(u.email) = lower(?) OR u.subject = ? ORDER BY u.id`,
		emailOrSubject, emailOrSubject)
	if err != nil {
		return nil, fmt.Errorf("error finding users: %w", err)
	}
	return collectOIDCUsers(rows, false)
}

// ListChainBindings returns every bound chain, with its user and when a
// device last talked to the server.
func (s *SQLite) ListChainBindings(ctx context.Context) ([]ChainBinding, error) {
	rows, err := s.r.QueryContext(ctx, `
		SELECT `+oidcUserColumns+`, c.client_id, c.created, c.session_expires,
			(SELECT MAX(last_active) FROM chains h WHERE lower(h.client_id) = c.client_id)
		FROM chain_logins c JOIN oidc_users u ON u.id = c.user_id
		ORDER BY u.email, c.client_id`)
	if err != nil {
		return nil, fmt.Errorf("error listing chains: %w", err)
	}
	defer rows.Close()
	var out []ChainBinding
	for rows.Next() {
		var b ChainBinding
		var created, expires, active sql.NullInt64
		u, err := scanOIDCUser(rows, &b.ClientID, &created, &expires, &active)
		if err != nil {
			return nil, fmt.Errorf("error reading chains: %w", err)
		}
		b.User, b.Created, b.SessionExpires = u, unixMs(created), unixMs(expires)
		if active.Valid {
			b.LastActive = time.Unix(active.Int64, 0)
		}
		out = append(out, b)
	}
	if err = rows.Err(); err != nil {
		return nil, fmt.Errorf("error reading chains: %w", err)
	}
	return out, nil
}

// StoredChainIDs returns the IDs chain |clientID| is stored under, in the
// case its devices sent (the sync tables keep it as sent).
func (s *SQLite) StoredChainIDs(ctx context.Context, clientID string) ([]string, error) {
	rows, err := s.r.QueryContext(ctx, `
		SELECT client_id FROM chains WHERE lower(client_id) = lower(?)
		UNION SELECT client_id FROM item_counts WHERE lower(client_id) = lower(?)`,
		clientID, clientID)
	if err != nil {
		return nil, fmt.Errorf("error finding chain: %w", err)
	}
	return collectStrings(rows)
}
