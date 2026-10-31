package cache

import (
	"context"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestMemoryClientRedisSemantics(t *testing.T) {
	ctx := context.Background()
	now := time.Unix(1000, 0)
	m := newMemoryClient(func() time.Time { return now })

	v, err := m.Get(ctx, "missing", false)
	require.NoError(t, err)
	assert.Empty(t, v, "a missing key reads as empty, as redis.Nil did")

	// TTL.
	require.NoError(t, m.Set(ctx, "k", "v", 10*time.Second))
	now = now.Add(9 * time.Second)
	v, _ = m.Get(ctx, "k", false)
	assert.Equal(t, "v", v)
	now = now.Add(time.Second)
	v, _ = m.Get(ctx, "k", false)
	assert.Empty(t, v, "expired")

	// INCR/DECR from nothing, GETDEL.
	n, err := m.Incr(ctx, "c", false)
	require.NoError(t, err)
	assert.Equal(t, 1, n)
	n, _ = m.Incr(ctx, "c", false)
	assert.Equal(t, 2, n)
	n, _ = m.Incr(ctx, "c", true)
	assert.Equal(t, 1, n)
	v, _ = m.Get(ctx, "c", true)
	assert.Equal(t, "1", v)
	v, _ = m.Get(ctx, "c", false)
	assert.Empty(t, v, "deleted by the get")

	// Del, FlushAll, and the sweep of expired keys.
	require.NoError(t, m.Set(ctx, "a", "1", 0))
	require.NoError(t, m.Set(ctx, "b", "2", time.Second))
	require.NoError(t, m.Del(ctx, "a"))
	v, _ = m.Get(ctx, "a", false)
	assert.Empty(t, v)
	now = now.Add(2 * time.Minute)
	require.NoError(t, m.Set(ctx, "x", "1", 0)) // Triggers the sweep.
	assert.NotContains(t, m.entries, "b")
	require.NoError(t, m.FlushAll(ctx))
	assert.Empty(t, m.entries)
}
