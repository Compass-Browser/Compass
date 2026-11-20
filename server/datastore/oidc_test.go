package datastore_test

// Company sign-in: users, chain bindings, schema 3.

import (
	"context"
	"database/sql"
	"path/filepath"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
)

const iss = "https://idp.example"

func TestOIDCBindings(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "o.db"), 0)
	ctx := context.Background()
	now := time.UnixMilli(1_800_000_000_000)

	b, err := db.ChainBinding(ctx, "abc")
	require.NoError(t, err)
	assert.Nil(t, b)
	assert.False(t, b.Valid(iss, now))

	u, err := db.SaveOIDCLogin(ctx, datastore.OIDCLogin{Issuer: iss, Subject: "s1", Email: "a@x.com",
		Name: "A", RefreshToken: "rt1"}, "ABC", time.Time{}, now)
	require.NoError(t, err)
	b, err = db.ChainBinding(ctx, "aBc")
	require.NoError(t, err)
	require.NotNil(t, b)
	assert.Equal(t, "abc", b.ClientID)
	assert.Equal(t, u.ID, b.User.ID)
	assert.Equal(t, "rt1", b.User.RefreshToken)
	assert.True(t, b.SessionExpires.IsZero())
	assert.True(t, b.Valid(iss, now.Add(1000*24*time.Hour)))
	assert.False(t, b.Valid("https://other", now))

	// A second sign-in without a refresh token keeps the first one's.
	_, err = db.SaveOIDCLogin(ctx, datastore.OIDCLogin{Issuer: iss, Subject: "s1", Email: "a@x.com", Name: "A2"},
		"def", now.Add(time.Hour), now)
	require.NoError(t, err)
	b, err = db.ChainBinding(ctx, "def")
	require.NoError(t, err)
	assert.Equal(t, "rt1", b.User.RefreshToken)
	assert.Equal(t, "A2", b.User.Name)
	assert.True(t, b.Valid(iss, now))
	assert.False(t, b.Valid(iss, now.Add(time.Hour)))

	ok, err := db.LinkChain(ctx, "ABC", "Profile", now)
	require.NoError(t, err)
	assert.True(t, ok)
	ok, err = db.LinkChain(ctx, "nobody", "x", now)
	require.NoError(t, err)
	assert.False(t, ok)

	users, err := db.OIDCUsersToRevalidate(ctx, iss, now.Add(time.Millisecond))
	require.NoError(t, err)
	require.Len(t, users, 1)
	users, err = db.OIDCUsersToRevalidate(ctx, iss, now)
	require.NoError(t, err)
	assert.Empty(t, users)
	require.NoError(t, db.UpdateOIDCUser(ctx, u.ID, "rt2", "", "Alice", now.Add(time.Hour)))
	users, err = db.FindOIDCUsers(ctx, "A@X.COM")
	require.NoError(t, err)
	require.Len(t, users, 1)
	assert.Equal(t, "rt2", users[0].RefreshToken)
	assert.Equal(t, "Alice", users[0].Name)
	assert.Equal(t, "a@x.com", users[0].Email)

	// Last activity comes from the chains table, in whatever case it's kept.
	require.NoError(t, db.TouchChain(ctx, "ABC", now))
	bindings, err := db.ListChainBindings(ctx)
	require.NoError(t, err)
	require.Len(t, bindings, 3)
	for _, b := range bindings {
		if b.ClientID == "abc" {
			assert.Equal(t, now.Unix(), b.LastActive.Unix())
		}
	}
	stored, err := db.StoredChainIDs(ctx, "abc")
	require.NoError(t, err)
	assert.Equal(t, []string{"ABC"}, stored)

	all, err := db.ListOIDCUsers(ctx)
	require.NoError(t, err)
	require.Len(t, all, 1)
	assert.Equal(t, 3, all[0].Chains)

	require.NoError(t, db.UnbindChain(ctx, "DEF"))
	chains, err := db.EndOIDCUserSessions(ctx, u.ID, true)
	require.NoError(t, err)
	assert.ElementsMatch(t, []string{"abc", "profile"}, chains)
	_, err = db.SaveOIDCLogin(ctx, datastore.OIDCLogin{Issuer: iss, Subject: "s1"}, "abc", time.Time{}, now)
	require.ErrorIs(t, err, datastore.ErrOIDCUserRevoked)
	b, err = db.ChainBinding(ctx, "abc")
	require.NoError(t, err)
	assert.Nil(t, b)
	revoked, err := db.OIDCUserRevoked(ctx, iss, "s1")
	require.NoError(t, err)
	assert.True(t, revoked)
	require.NoError(t, db.RestoreOIDCUser(ctx, u.ID))
	_, err = db.SaveOIDCLogin(ctx, datastore.OIDCLogin{Issuer: iss, Subject: "s1"}, "abc", time.Time{}, now)
	require.NoError(t, err)
}

// A database made by schema 2 gets the sign-in tables and keeps its data.
func TestMigrateSchema2(t *testing.T) {
	path := filepath.Join(t.TempDir(), "v2.db")
	db := openSQLite(t, path, 0)
	ctx := context.Background()
	require.NoError(t, db.TouchChain(ctx, "old", time.Now()))
	require.NoError(t, db.Close())
	raw, err := sql.Open("sqlite", "file:"+path)
	require.NoError(t, err)
	_, err = raw.Exec(`DROP TABLE chain_logins; DROP TABLE oidc_users; PRAGMA user_version = 2;`)
	require.NoError(t, err)
	require.NoError(t, raw.Close())

	db = openSQLite(t, path, 0)
	last, err := db.LastActive(ctx, "old")
	require.NoError(t, err)
	assert.WithinDuration(t, time.Now(), last, time.Minute)
	_, err = db.SaveOIDCLogin(ctx, datastore.OIDCLogin{Issuer: iss, Subject: "s"}, "old", time.Time{}, time.Now())
	require.NoError(t, err)
	require.NoError(t, db.Close())

	raw, err = sql.Open("sqlite", "file:"+path)
	require.NoError(t, err)
	defer raw.Close()
	var version int
	require.NoError(t, raw.QueryRow(`PRAGMA user_version`).Scan(&version))
	assert.Equal(t, 4, version) // The current schema.
}
