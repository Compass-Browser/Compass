package cache

// Compass: an in-process replacement for go-sync's Redis client. go-sync
// runs many instances behind one Redis; the Compass server is one process
// (it owns its SQLite file), so a map does the same job. It holds two
// things: each type's last mtime per chain (lets periodic polls skip the
// database) and the item counts of commits in progress (feeds the quota).
// Both are rebuilt from the database after a restart.

import (
	"context"
	"strconv"
	"sync"
	"time"
)

// RedisClient is the cache's storage (the name is go-sync's).
type RedisClient interface {
	Set(ctx context.Context, key string, val string, ttl time.Duration) error
	Incr(ctx context.Context, key string, subtract bool) (int, error)
	Get(ctx context.Context, key string, deleteAfterGet bool) (string, error)
	Del(ctx context.Context, keys ...string) error
	FlushAll(ctx context.Context) error
}

type memoryEntry struct {
	val     string
	expires time.Time // Zero: never.
}

// memoryClient is a RedisClient in a map, with Redis's semantics for the
// calls the cache makes.
type memoryClient struct {
	mu        sync.Mutex
	entries   map[string]memoryEntry
	now       func() time.Time
	lastSweep time.Time
}

// sweepInterval: how often expired keys are dropped, so keys of chains that
// stop polling don't pile up.
const sweepInterval = time.Minute

// NewMemoryClient returns an empty in-process cache.
func NewMemoryClient() RedisClient {
	return newMemoryClient(time.Now)
}

func newMemoryClient(now func() time.Time) *memoryClient {
	return &memoryClient{entries: map[string]memoryEntry{}, now: now}
}

// get returns the live entry for key; the caller holds mu.
func (m *memoryClient) get(key string, now time.Time) (memoryEntry, bool) {
	e, ok := m.entries[key]
	if ok && !e.expires.IsZero() && !now.Before(e.expires) {
		delete(m.entries, key)
		return memoryEntry{}, false
	}
	return e, ok
}

func (m *memoryClient) maybeSweep(now time.Time) {
	if now.Sub(m.lastSweep) < sweepInterval {
		return
	}
	m.lastSweep = now
	for k, e := range m.entries {
		if !e.expires.IsZero() && !now.Before(e.expires) {
			delete(m.entries, k)
		}
	}
}

func (m *memoryClient) Set(_ context.Context, key string, val string, ttl time.Duration) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	now := m.now()
	m.maybeSweep(now)
	e := memoryEntry{val: val}
	if ttl > 0 {
		e.expires = now.Add(ttl)
	}
	m.entries[key] = e
	return nil
}

// Incr adds or subtracts 1, starting from 0, keeping any expiry (as Redis's
// INCR/DECR do).
func (m *memoryClient) Incr(_ context.Context, key string, subtract bool) (int, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	e, _ := m.get(key, m.now())
	n := 0
	if e.val != "" {
		var err error
		if n, err = strconv.Atoi(e.val); err != nil {
			return 0, err
		}
	}
	if subtract {
		n--
	} else {
		n++
	}
	e.val = strconv.Itoa(n)
	m.entries[key] = e
	return n, nil
}

// Get returns "" for a missing key (go-sync maps redis.Nil to "").
func (m *memoryClient) Get(_ context.Context, key string, deleteAfterGet bool) (string, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	e, ok := m.get(key, m.now())
	if ok && deleteAfterGet {
		delete(m.entries, key)
	}
	return e.val, nil
}

func (m *memoryClient) Del(_ context.Context, keys ...string) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, k := range keys {
		delete(m.entries, k)
	}
	return nil
}

func (m *memoryClient) FlushAll(_ context.Context) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.entries = map[string]memoryEntry{}
	return nil
}
