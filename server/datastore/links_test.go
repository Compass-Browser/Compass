package datastore_test

// Linked chains: a private server's profile accounts (sync.md, "Profiles").

import (
	"context"
	"fmt"
	"path/filepath"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
)

func TestChainLinks(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "l.db"), 0)
	ctx := context.Background()
	now := time.UnixMilli(1_800_000_000_000)

	root, err := db.ChainLinkRoot(ctx, "work")
	require.NoError(t, err)
	assert.Empty(t, root)

	// Home links Work; Work links Side: both point at Home.
	require.NoError(t, db.LinkChainToRoot(ctx, "HOME", "Work", now))
	require.NoError(t, db.LinkChainToRoot(ctx, "work", "side", now))
	for _, c := range []string{"work", "WORK", "side"} {
		root, err = db.ChainLinkRoot(ctx, c)
		require.NoError(t, err)
		assert.Equal(t, "home", root, c)
	}
	// Linking the root, a chain to itself or again changes nothing.
	require.NoError(t, db.LinkChainToRoot(ctx, "work", "home", now))
	require.NoError(t, db.LinkChainToRoot(ctx, "home", "home", now))
	require.NoError(t, db.LinkChainToRoot(ctx, "home", "work", now))
	root, err = db.ChainLinkRoot(ctx, "home")
	require.NoError(t, err)
	assert.Empty(t, root)

	// Another root takes a chain over (its seed moved to another chain).
	require.NoError(t, db.LinkChainToRoot(ctx, "other", "side", now))
	root, err = db.ChainLinkRoot(ctx, "side")
	require.NoError(t, err)
	assert.Equal(t, "other", root)

	// At most MaxLinksPerChain per root.
	for i := 0; i < datastore.MaxLinksPerChain; i++ {
		err = db.LinkChainToRoot(ctx, "busy", fmt.Sprintf("p%d", i), now)
		require.NoError(t, err)
	}
	assert.ErrorIs(t, db.LinkChainToRoot(ctx, "busy", "one-more", now), datastore.ErrTooManyLinks)
	// Already linked there: fine.
	require.NoError(t, db.LinkChainToRoot(ctx, "busy", "p0", now))
}
