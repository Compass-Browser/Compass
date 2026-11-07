// Package pairing relays the messages two Compass devices exchange to add
// a device to a sync chain (docs/design/sync.md, "Pairing API"). Device A,
// already syncing, opens a numbered slot; device B joins it by number and
// both post and long-poll opaque messages (SPAKE2, confirmations, the
// encrypted seed). The server can't read them; it only enforces who may
// post and read, sizes, counts and lifetimes. Everything is in memory: a
// restart drops open slots, and the devices start again.
//
//	POST   /             A opens a slot (sync token)        201 {"slot","expires_in","server"}
//	POST   /{slot}/join  B joins (no auth)                  200 {"b_token","server","expires_in"}
//	POST   /{slot}/msg   {"from":"a"|"b","data":base64}     200 {"seq"}
//	GET    /{slot}/msg?for=a|b&after=n  long poll           200 {"messages":[{"seq","data"}]}
//	DELETE /{slot}?reason=mismatch|cancel|deny
//	                     A's token or B's b_token           204
//
// A closed slot says why (sync.md, "Pairing API"): for ClosedTTL after it
// closes, every request on its number answers 410 {"reason": "mismatch" |
// "cancel" | "deny" | "expired"}, and the number isn't given out again
// meanwhile. "server" is the canonical name both devices bind into SPAKE2
// (Config.PublicHost, else the Host the opening request came to).
package pairing

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/go-chi/chi/v5"
)

// PairTokenHeader carries B's b_token.
const PairTokenHeader = "X-Compass-Pair-Token"

// Config sets the relay's rules. DefaultConfig has the contract's values.
type Config struct {
	SlotTTL            time.Duration // A slot's life from opening.
	PollTimeout        time.Duration // How long a GET waits for a message.
	MaxMessagesPerSide int
	MaxMessageBytes    int // Decoded size of one message's data.
	JoinsPerIP         int // Join attempts per IP per JoinWindow.
	JoinWindow         time.Duration
	OpensPerIP         int // Slot openings per IP per JoinWindow (server policy).
	MaxOpenSlots       int // All chains together (server policy).
	// How long a closed or expired slot's number answers 410 with the
	// reason (and stays reserved).
	ClosedTTL time.Duration
	// The server's canonical host name, bound into SPAKE2 by both devices
	// (COMPASS_SYNC_PUBLIC_HOST). Empty: the Host of the request that opens
	// the slot.
	PublicHost string

	// Now is the clock for slot expiry and rate limits.
	Now func() time.Time
	// ClientIP returns the client address used for rate limits.
	ClientIP func(*http.Request) string
	// Authorize checks a sync token, returning the chain ID (auth.Authorize).
	Authorize func(*http.Request) (string, error)
}

// DefaultConfig returns the contract's limits. ClientIP and Authorize must
// still be set.
func DefaultConfig() Config {
	return Config{
		SlotTTL:            300 * time.Second,
		PollTimeout:        25 * time.Second,
		MaxMessagesPerSide: 8,
		MaxMessageBytes:    4096,
		JoinsPerIP:         10,
		JoinWindow:         10 * time.Minute,
		OpensPerIP:         30,
		MaxOpenSlots:       10000,
		ClosedTTL:          300 * time.Second,
		Now:                time.Now,
	}
}

const (
	sideA = 0
	sideB = 1
)

// Why a slot closed, as DELETE's ?reason= and the 410 reply name it.
const (
	ReasonMismatch = "mismatch" // A confirmation didn't match: a wrong code.
	ReasonCancel   = "cancel"   // A or B gave up, or A opened a new slot.
	ReasonDeny     = "deny"     // A's user didn't add B.
	ReasonExpired  = "expired"
)

// closedSlot remembers why a slot number closed, until |until|.
type closedSlot struct {
	reason string
	until  time.Time
}

type slot struct {
	num     int
	chain   string
	server  string // Canonical host name, bound into SPAKE2.
	expires time.Time
	bToken  string        // Empty until B joins.
	msgs    [2][]string   // Base64 data, by sender.
	changed chan struct{} // Closed (and replaced) on a message or close.
}

// Server is the pairing relay.
type Server struct {
	cfg Config

	mu        sync.Mutex
	slots     map[int]*slot
	closed    map[int]closedSlot
	byChain   map[string]int
	joins     map[string][]time.Time
	opens     map[string][]time.Time
	lastSweep time.Time
}

// New returns a relay with |cfg|.
func New(cfg Config) *Server {
	return &Server{
		cfg:     cfg,
		slots:   map[int]*slot{},
		closed:  map[int]closedSlot{},
		byChain: map[string]int{},
		joins:   map[string][]time.Time{},
		opens:   map[string][]time.Time{},
	}
}

// Routes returns the relay's handler, to mount at /v2/compass/pair.
func (s *Server) Routes() http.Handler {
	r := chi.NewRouter()
	r.Post("/", s.open)
	r.Post("/{slot}/join", s.join)
	r.Post("/{slot}/msg", s.post)
	r.Get("/{slot}/msg", s.poll)
	r.Delete("/{slot}", s.close)
	return r
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

// chainKey compares chain IDs (hex public keys) case-insensitively: Chromium
// sends upper-case hex, other clients may not.
func chainKey(id string) string { return strings.ToLower(id) }

func (s *Server) expired(sl *slot, now time.Time) bool {
	return !now.Before(sl.expires)
}

// closeLocked removes a slot, remembers why (from |at|), and wakes its
// pollers.
func (s *Server) closeLocked(sl *slot, reason string, at time.Time) {
	delete(s.slots, sl.num)
	if s.cfg.ClosedTTL > 0 {
		s.closed[sl.num] = closedSlot{reason: reason, until: at.Add(s.cfg.ClosedTTL)}
	}
	if s.byChain[sl.chain] == sl.num {
		delete(s.byChain, sl.chain)
	}
	close(sl.changed)
	sl.changed = nil
}

// lookupLocked returns the open slot |num|, or nil (unknown, closed or
// expired) and, if it closed lately, why.
func (s *Server) lookupLocked(num int, now time.Time) (*slot, string) {
	sl := s.slots[num]
	if sl != nil && s.expired(sl, now) {
		s.closeLocked(sl, ReasonExpired, sl.expires)
		sl = nil
	}
	if sl != nil {
		return sl, ""
	}
	return nil, s.closedReasonLocked(num, now)
}

// closedReasonLocked is why slot |num| closed, if it did within ClosedTTL.
func (s *Server) closedReasonLocked(num int, now time.Time) string {
	c, ok := s.closed[num]
	if !ok {
		return ""
	}
	if !now.Before(c.until) {
		delete(s.closed, num)
		return ""
	}
	return c.reason
}

// gone answers a request on a slot that isn't open: 410 with the reason if
// it closed lately, else 404.
func gone(w http.ResponseWriter, reason string) {
	if reason != "" {
		writeJSON(w, http.StatusGone, map[string]string{
			"error":  "pairing slot closed",
			"reason": reason,
		})
		return
	}
	fail(w, http.StatusNotFound, "no such pairing slot")
}

// canonicalHost is the name both devices bind into SPAKE2: PublicHost if
// set, else the Host the request came to; without a port, lower-case (as
// Chromium's GURL::host(), which older clients bind).
func (s *Server) canonicalHost(r *http.Request) string {
	h := s.cfg.PublicHost
	if h == "" {
		h = r.Host
	}
	if host, _, err := net.SplitHostPort(h); err == nil {
		h = host
	}
	return strings.ToLower(h)
}

// secondsLeft is a slot's remaining life in whole seconds, rounded up.
func secondsLeft(sl *slot, now time.Time) int {
	left := sl.expires.Sub(now)
	return int((left + time.Second - 1) / time.Second)
}

// sweepLocked drops expired slots and old rate-limit entries, at most once
// a minute.
func (s *Server) sweepLocked(now time.Time) {
	if now.Sub(s.lastSweep) < time.Minute {
		return
	}
	s.lastSweep = now
	for _, sl := range s.slots {
		if s.expired(sl, now) {
			s.closeLocked(sl, ReasonExpired, sl.expires)
		}
	}
	for num, c := range s.closed {
		if !now.Before(c.until) {
			delete(s.closed, num)
		}
	}
	for _, m := range []map[string][]time.Time{s.joins, s.opens} {
		for ip, times := range m {
			if kept := s.recent(times, now); len(kept) == 0 {
				delete(m, ip)
			} else {
				m[ip] = kept
			}
		}
	}
}

func (s *Server) recent(times []time.Time, now time.Time) []time.Time {
	cutoff := now.Add(-s.cfg.JoinWindow)
	i := 0
	for i < len(times) && !times[i].After(cutoff) {
		i++
	}
	return times[i:]
}

// allowLocked records an attempt from |ip| in |m| and says whether it is
// within |limit| per window.
func (s *Server) allowLocked(m map[string][]time.Time, ip string, limit int, now time.Time) bool {
	times := s.recent(m[ip], now)
	if len(times) >= limit {
		m[ip] = times
		return false
	}
	m[ip] = append(times, now)
	return true
}

func slotNumber(r *http.Request) (int, bool) {
	n, err := strconv.Atoi(chi.URLParam(r, "slot"))
	return n, err == nil && n >= 1
}

func (s *Server) open(w http.ResponseWriter, r *http.Request) {
	chain, err := s.cfg.Authorize(r)
	if err != nil || chain == "" {
		fail(w, http.StatusUnauthorized, "invalid sync token")
		return
	}
	ip := s.cfg.ClientIP(r)

	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.cfg.Now()
	s.sweepLocked(now)
	if !s.allowLocked(s.opens, ip, s.cfg.OpensPerIP, now) {
		fail(w, http.StatusTooManyRequests, "too many pairing slots opened")
		return
	}
	key := chainKey(chain)
	if old := s.slots[s.byChain[key]]; old != nil && old.chain == key {
		if s.expired(old, now) {
			s.closeLocked(old, ReasonExpired, old.expires)
		} else {
			s.closeLocked(old, ReasonCancel, now)
		}
	}
	if len(s.slots) >= s.cfg.MaxOpenSlots {
		fail(w, http.StatusServiceUnavailable, "too many open pairing slots")
		return
	}
	// The lowest number neither open nor closed lately: a device that
	// typed a closed slot's code learns why instead of reaching a new slot.
	num := 1
	for {
		sl, reason := s.lookupLocked(num, now)
		if sl == nil && reason == "" {
			break
		}
		num++
	}
	sl := &slot{
		num:     num,
		chain:   key,
		server:  s.canonicalHost(r),
		expires: now.Add(s.cfg.SlotTTL),
		changed: make(chan struct{}),
	}
	s.slots[num] = sl
	s.byChain[key] = num
	writeJSON(w, http.StatusCreated, map[string]any{
		"slot":       num,
		"expires_in": int(s.cfg.SlotTTL / time.Second),
		"server":     sl.server,
	})
}

func (s *Server) join(w http.ResponseWriter, r *http.Request) {
	ip := s.cfg.ClientIP(r)
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.cfg.Now()
	s.sweepLocked(now)
	// Every attempt counts, found or not: guessing slots is what's limited.
	if !s.allowLocked(s.joins, ip, s.cfg.JoinsPerIP, now) {
		fail(w, http.StatusTooManyRequests, "too many join attempts")
		return
	}
	num, ok := slotNumber(r)
	var sl *slot
	var reason string
	if ok {
		sl, reason = s.lookupLocked(num, now)
	}
	if sl == nil {
		gone(w, reason)
		return
	}
	if sl.bToken != "" {
		fail(w, http.StatusConflict, "pairing slot already joined")
		return
	}
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		fail(w, http.StatusInternalServerError, "no randomness")
		return
	}
	sl.bToken = hex.EncodeToString(b[:])
	writeJSON(w, http.StatusOK, map[string]any{
		"b_token":    sl.bToken,
		"server":     sl.server,
		"expires_in": secondsLeft(sl, now),
	})
}

// caller is who a request proves to be.
type caller struct {
	chain  string // From a valid Authorization, lower-case.
	bToken string // From X-Compass-Pair-Token.
}

func (s *Server) identify(r *http.Request) caller {
	var c caller
	if r.Header.Get("Authorization") != "" {
		if chain, err := s.cfg.Authorize(r); err == nil {
			c.chain = chainKey(chain)
		}
	}
	c.bToken = r.Header.Get(PairTokenHeader)
	return c
}

// authorizeLocked checks |c| may act as |side| on |sl|, returning the HTTP
// status to answer if not (0 if allowed).
func authorizeLocked(sl *slot, side int, c caller) int {
	if side == sideA {
		if c.chain == "" {
			return http.StatusUnauthorized
		}
		if c.chain != sl.chain {
			return http.StatusForbidden
		}
		return 0
	}
	if c.bToken == "" {
		return http.StatusUnauthorized
	}
	if sl.bToken == "" || subtle.ConstantTimeCompare([]byte(c.bToken), []byte(sl.bToken)) != 1 {
		return http.StatusForbidden
	}
	return 0
}

func parseSide(v string) (int, bool) {
	switch v {
	case "a":
		return sideA, true
	case "b":
		return sideB, true
	}
	return 0, false
}

type postBody struct {
	From string `json:"from"`
	Data string `json:"data"`
}

func (s *Server) post(w http.ResponseWriter, r *http.Request) {
	num, ok := slotNumber(r)
	if !ok {
		fail(w, http.StatusNotFound, "no such pairing slot")
		return
	}
	// Base64 of the largest message plus the JSON around it.
	limit := int64(base64.StdEncoding.EncodedLen(s.cfg.MaxMessageBytes) + 1024)
	raw, err := io.ReadAll(io.LimitReader(r.Body, limit+1))
	if err != nil {
		fail(w, http.StatusBadRequest, "unreadable body")
		return
	}
	if int64(len(raw)) > limit {
		fail(w, http.StatusRequestEntityTooLarge, "message too large")
		return
	}
	var body postBody
	if err = json.Unmarshal(raw, &body); err != nil {
		fail(w, http.StatusBadRequest, "body must be JSON {from, data}")
		return
	}
	side, ok := parseSide(body.From)
	if !ok {
		fail(w, http.StatusBadRequest, `"from" must be "a" or "b"`)
		return
	}
	data, err := base64.StdEncoding.DecodeString(body.Data)
	if err != nil || len(data) == 0 {
		fail(w, http.StatusBadRequest, `"data" must be non-empty standard base64`)
		return
	}
	if len(data) > s.cfg.MaxMessageBytes {
		fail(w, http.StatusRequestEntityTooLarge, "message too large")
		return
	}
	c := s.identify(r)

	s.mu.Lock()
	defer s.mu.Unlock()
	sl, reason := s.lookupLocked(num, s.cfg.Now())
	if sl == nil {
		gone(w, reason)
		return
	}
	if status := authorizeLocked(sl, side, c); status != 0 {
		fail(w, status, "not allowed to post as "+body.From)
		return
	}
	if len(sl.msgs[side]) >= s.cfg.MaxMessagesPerSide {
		fail(w, http.StatusTooManyRequests, "too many messages")
		return
	}
	sl.msgs[side] = append(sl.msgs[side], base64.StdEncoding.EncodeToString(data))
	close(sl.changed)
	sl.changed = make(chan struct{})
	writeJSON(w, http.StatusOK, map[string]int{"seq": len(sl.msgs[side])})
}

type message struct {
	Seq  int    `json:"seq"`
	Data string `json:"data"`
}

// errGone carries why a polled slot closed ("" if unknown).
type errGone struct{ reason string }

func (e errGone) Error() string { return "gone: " + e.reason }

func (s *Server) poll(w http.ResponseWriter, r *http.Request) {
	num, ok := slotNumber(r)
	if !ok {
		fail(w, http.StatusNotFound, "no such pairing slot")
		return
	}
	reader, ok := parseSide(r.URL.Query().Get("for"))
	if !ok {
		fail(w, http.StatusBadRequest, `"for" must be "a" or "b"`)
		return
	}
	after := 0
	if v := r.URL.Query().Get("after"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 {
			fail(w, http.StatusBadRequest, `"after" must be a sequence number`)
			return
		}
		after = n
	}
	sender := sideB
	if reader == sideB {
		sender = sideA
	}
	c := s.identify(r)
	deadline := time.NewTimer(s.cfg.PollTimeout)
	defer deadline.Stop()

	// check returns the messages to answer with, or a channel to wait on.
	check := func() ([]message, <-chan struct{}, time.Duration, int, error) {
		s.mu.Lock()
		defer s.mu.Unlock()
		now := s.cfg.Now()
		sl, reason := s.lookupLocked(num, now)
		if sl == nil {
			return nil, nil, 0, 0, errGone{reason}
		}
		if status := authorizeLocked(sl, reader, c); status != 0 {
			return nil, nil, 0, status, nil
		}
		var out []message
		for i := after; i < len(sl.msgs[sender]); i++ {
			out = append(out, message{Seq: i + 1, Data: sl.msgs[sender][i]})
		}
		return out, sl.changed, sl.expires.Sub(now), 0, nil
	}

	for {
		msgs, changed, untilExpiry, status, err := check()
		var g errGone
		if errors.As(err, &g) {
			gone(w, g.reason)
			return
		}
		if status != 0 {
			fail(w, status, "not allowed to read")
			return
		}
		if len(msgs) > 0 {
			writeJSON(w, http.StatusOK, map[string][]message{"messages": msgs})
			return
		}
		expiry := time.NewTimer(untilExpiry)
		select {
		case <-changed:
			expiry.Stop()
		case <-expiry.C:
		case <-deadline.C:
			expiry.Stop()
			writeJSON(w, http.StatusOK, map[string][]message{"messages": {}})
			return
		case <-r.Context().Done():
			expiry.Stop()
			return
		}
	}
}

func (s *Server) close(w http.ResponseWriter, r *http.Request) {
	num, ok := slotNumber(r)
	if !ok {
		fail(w, http.StatusNotFound, "no such pairing slot")
		return
	}
	// Older clients send no reason: a plain cancel.
	why := r.URL.Query().Get("reason")
	switch why {
	case "":
		why = ReasonCancel
	case ReasonMismatch, ReasonCancel, ReasonDeny:
	default:
		fail(w, http.StatusBadRequest, `"reason" must be "mismatch", "cancel" or "deny"`)
		return
	}
	c := s.identify(r)
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.cfg.Now()
	sl, reason := s.lookupLocked(num, now)
	if sl == nil {
		gone(w, reason)
		return
	}
	statusA := authorizeLocked(sl, sideA, c)
	statusB := authorizeLocked(sl, sideB, c)
	if statusA != 0 && statusB != 0 {
		// 401 when no credential was sent, 403 when a wrong one was.
		fail(w, max(statusA, statusB), "not allowed to close")
		return
	}
	s.closeLocked(sl, why, now)
	w.WriteHeader(http.StatusNoContent)
}
