package datastore

// Compass: strictly increasing mtimes.
//
// An entity's mtime (ms) is its version and the progress token of the
// GetUpdates that returns it last: the next request asks for mtime > token.
// Two entities with the same mtime split across two pages would lose the
// second one. go-sync on DynamoDB rarely saw equal mtimes (each write was a
// network round trip); a local SQLite write takes microseconds, so a
// commit's entries would share a millisecond. The server is one process
// owning its database, so one clock hands out unique mtimes: the current
// time, or the last one + 1 when that isn't later. Bursts run the clock a
// little ahead of real time (1000 entities per second), and it falls back
// to real time when idle.

import (
	"sync"
	"time"
)

var mtimeClock struct {
	sync.Mutex
	last int64
}

// NextMtime returns a new mtime in Unix milliseconds, later than every
// mtime returned before (and than every mtime in the opened database).
func NextMtime() int64 {
	mtimeClock.Lock()
	defer mtimeClock.Unlock()
	now := time.Now().UnixMilli()
	if now <= mtimeClock.last {
		now = mtimeClock.last + 1
	}
	mtimeClock.last = now
	return now
}

// observeMtime makes NextMtime return values after m (an mtime read from
// the database at startup, which may be ahead of the clock).
func observeMtime(m int64) {
	mtimeClock.Lock()
	defer mtimeClock.Unlock()
	if m > mtimeClock.last {
		mtimeClock.last = m
	}
}
