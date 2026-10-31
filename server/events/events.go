// Package events pushes change notifications to a chain's devices
// (docs/design/sync.md, "Change notifications", slice 6). A device keeps
// one Server-Sent Events stream open; when another device of its chain
// commits, the server sends the data types that changed, never contents,
// and the device asks for updates at once instead of waiting for its poll.
//
//	GET /?client_id=<cache GUID>   (sync token)   200 text/event-stream
//
//	: connected
//
//	event: changed
//	data: {"types":[32904,970000]}
//
//	: keep-alive
//
// The cache GUID names the device so that it isn't told about its own
// commits; without it the device also hears its own (harmless). Everything
// is in memory: a restart drops the streams, and the devices reconnect.
package events

import (
	"encoding/json"
	"errors"
	"net/http"
	"slices"
	"strings"
	"sync"
	"time"
)

// Config sets the stream's rules. DefaultConfig has the values the server
// runs with.
type Config struct {
	KeepAlive    time.Duration // A comment line this often keeps proxies and NATs from closing the stream.
	WriteTimeout time.Duration // A write that takes longer ends the stream (a stuck client).
	MaxPerChain  int           // Open streams per chain.
	MaxPerIP     int           // Open streams per client address.
	MaxClientID  int           // Longest accepted client_id.

	// Authorize checks a sync token, returning the chain ID (auth.Authorize).
	Authorize func(*http.Request) (string, error)
	// IsDisabled says whether a chain was disabled (as the command endpoint's
	// DisabledChain middleware). Optional.
	IsDisabled func(r *http.Request, chain string) (bool, error)
	// ClientIP returns the client address used for the per-address limit.
	ClientIP func(*http.Request) string
	// NewTicker makes the keep-alive ticker; tests replace it. It returns the
	// tick channel and a stop function.
	NewTicker func(time.Duration) (<-chan time.Time, func())
}

// DefaultConfig returns the server's limits. Authorize and ClientIP must
// still be set.
func DefaultConfig() Config {
	return Config{
		KeepAlive:    25 * time.Second,
		WriteTimeout: 15 * time.Second,
		MaxPerChain:  20,
		MaxPerIP:     50,
		MaxClientID:  128,
		NewTicker: func(d time.Duration) (<-chan time.Time, func()) {
			t := time.NewTicker(d)
			return t.C, t.Stop
		},
	}
}

// listener is one open stream.
type listener struct {
	chain  string
	client string
	ip     string

	mu      sync.Mutex
	pending map[int]struct{} // Types changed since the last event was written.
	wake    chan struct{}    // Capacity 1: "pending has something".
}

// Hub holds the open streams by chain and sends them change notifications.
type Hub struct {
	cfg Config

	mu      sync.Mutex
	byChain map[string]map[*listener]struct{}
	byIP    map[string]int
	closed  chan struct{}
	isShut  bool
}

// New returns a hub with |cfg|.
func New(cfg Config) *Hub {
	return &Hub{
		cfg:     cfg,
		byChain: map[string]map[*listener]struct{}{},
		byIP:    map[string]int{},
		closed:  make(chan struct{}),
	}
}

// chainKey compares chain IDs (hex public keys) case-insensitively: Chromium
// sends upper-case hex, other clients may not.
func chainKey(id string) string { return strings.ToLower(id) }

var (
	errChainLimit = errors.New("too many streams for this chain")
	errIPLimit    = errors.New("too many streams from this address")
	errShutdown   = errors.New("shutting down")
)

func (h *Hub) add(l *listener) error {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.isShut {
		return errShutdown
	}
	set := h.byChain[l.chain]
	if len(set) >= h.cfg.MaxPerChain {
		return errChainLimit
	}
	if h.byIP[l.ip] >= h.cfg.MaxPerIP {
		return errIPLimit
	}
	if set == nil {
		set = map[*listener]struct{}{}
		h.byChain[l.chain] = set
	}
	set[l] = struct{}{}
	h.byIP[l.ip]++
	return nil
}

func (h *Hub) remove(l *listener) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if set := h.byChain[l.chain]; set != nil {
		delete(set, l)
		if len(set) == 0 {
			delete(h.byChain, l.chain)
		}
	}
	if h.byIP[l.ip]--; h.byIP[l.ip] <= 0 {
		delete(h.byIP, l.ip)
	}
}

// Count returns the open streams of |chain| (for tests and metrics).
func (h *Hub) Count(chain string) int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.byChain[chainKey(chain)])
}

// Notify tells |chain|'s streams, except those of the device |exceptClient|
// (its cache GUID; empty excludes nobody), that |types| changed. It never
// blocks: a stream that hasn't written the last event yet gets the types
// merged into its next one.
func (h *Hub) Notify(chain, exceptClient string, types []int) {
	if len(types) == 0 {
		return
	}
	h.mu.Lock()
	var targets []*listener
	for l := range h.byChain[chainKey(chain)] {
		if exceptClient != "" && l.client == exceptClient {
			continue
		}
		targets = append(targets, l)
	}
	h.mu.Unlock()
	for _, l := range targets {
		l.mu.Lock()
		for _, t := range types {
			l.pending[t] = struct{}{}
		}
		l.mu.Unlock()
		select {
		case l.wake <- struct{}{}:
		default:
		}
	}
}

// Close ends every stream and refuses new ones (graceful shutdown: pass it
// to http.Server.RegisterOnShutdown, since Shutdown doesn't end handlers).
func (h *Hub) Close() {
	h.mu.Lock()
	defer h.mu.Unlock()
	if !h.isShut {
		h.isShut = true
		close(h.closed)
	}
}

func (l *listener) takePending() []int {
	l.mu.Lock()
	defer l.mu.Unlock()
	types := make([]int, 0, len(l.pending))
	for t := range l.pending {
		types = append(types, t)
	}
	clear(l.pending)
	slices.Sort(types)
	return types
}

type changedEvent struct {
	Types []int `json:"types"`
}

func fail(w http.ResponseWriter, status int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

// ServeHTTP serves one stream until the client goes, a write fails or the
// hub closes. Mount it outside any request timeout middleware.
func (h *Hub) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		fail(w, http.StatusMethodNotAllowed, "GET only")
		return
	}
	chain, err := h.cfg.Authorize(r)
	if err != nil || chain == "" {
		fail(w, http.StatusUnauthorized, "invalid sync token")
		return
	}
	if h.cfg.IsDisabled != nil {
		disabled, err := h.cfg.IsDisabled(r, chain)
		if err != nil {
			fail(w, http.StatusInternalServerError, "unable to complete request")
			return
		}
		if disabled {
			fail(w, http.StatusForbidden, "sync chain disabled")
			return
		}
	}
	client := r.URL.Query().Get("client_id")
	if len(client) > h.cfg.MaxClientID {
		fail(w, http.StatusBadRequest, "client_id too long")
		return
	}
	rc := http.NewResponseController(w)

	l := &listener{
		chain:   chainKey(chain),
		client:  client,
		ip:      h.cfg.ClientIP(r),
		pending: map[int]struct{}{},
		wake:    make(chan struct{}, 1),
	}
	switch err := h.add(l); {
	case errors.Is(err, errShutdown):
		fail(w, http.StatusServiceUnavailable, err.Error())
		return
	case err != nil:
		w.Header().Set("Retry-After", "60")
		fail(w, http.StatusTooManyRequests, err.Error())
		return
	}
	defer h.remove(l)

	hdr := w.Header()
	hdr.Set("Content-Type", "text/event-stream")
	hdr.Set("Cache-Control", "no-store")
	// nginx and some other proxies buffer responses unless told not to.
	hdr.Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)
	if h.cfg.WriteTimeout > 0 {
		// The server sets no write deadline of its own; don't leave ours on
		// the connection.
		defer func() { _ = rc.SetWriteDeadline(time.Time{}) }()
	}

	write := func(s string) bool {
		if h.cfg.WriteTimeout > 0 {
			_ = rc.SetWriteDeadline(time.Now().Add(h.cfg.WriteTimeout))
		}
		if _, err := w.Write([]byte(s)); err != nil {
			return false
		}
		return rc.Flush() == nil
	}
	// The first bytes: the client knows the stream is up (and, behind a
	// buffering proxy, sees at once that it isn't).
	if !write(": connected\n\n") {
		return
	}

	tick, stop := h.cfg.NewTicker(h.cfg.KeepAlive)
	defer stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case <-h.closed:
			return
		case <-tick:
			if !write(": keep-alive\n\n") {
				return
			}
		case <-l.wake:
			types := l.takePending()
			if len(types) == 0 {
				continue
			}
			data, _ := json.Marshal(changedEvent{Types: types})
			if !write("event: changed\ndata: " + string(data) + "\n\n") {
				return
			}
		}
	}
}
