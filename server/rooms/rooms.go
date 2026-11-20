// Package rooms serves rooms: spaces, folders and easels shared between
// users (docs/design/sync.md, "Rooms", which documents every route
// exactly). A room is a set of records the server stores and orders but
// can't read (each is encrypted with the room key of an epoch), and its
// members, chain accounts with a role: owner, editor or viewer. Room keys
// reach members as envelopes sealed to their identity keys (X25519), which
// the server stores opaquely.
//
//	GET    /v2/compass/rooms                            my rooms
//	POST   /v2/compass/rooms                            create (caller is the owner)
//	GET    /v2/compass/rooms/{room}                     member: the room, members, my envelopes
//	DELETE /v2/compass/rooms/{room}                     owner
//	GET    /v2/compass/rooms/{room}/changes             joined member: records after ?since (long poll)
//	POST   /v2/compass/rooms/{room}/records             editor or owner
//	POST   /v2/compass/rooms/{room}/invites             owner: link invite
//	DELETE /v2/compass/rooms/{room}/invites/{invite}    owner: revoke
//	POST   /v2/compass/rooms/{room}/join                invite holder, or a pending member
//	POST   /v2/compass/rooms/{room}/members             owner: email invite (company sign-in only)
//	PATCH  /v2/compass/rooms/{room}/members/{client}    owner (role), the member itself (profile)
//	DELETE /v2/compass/rooms/{room}/members/{client}    owner, or the member itself (leave)
//	POST   /v2/compass/rooms/{room}/owner               owner: transfer
//	POST   /v2/compass/rooms/{room}/epochs              owner: rotate the key
//	POST   /v2/compass/rooms/{room}/envelopes           owner: keys of past epochs for members
//	GET    /v2/compass/identity, PUT                    the caller's identity public key
//	GET    /v2/compass/directory?email=                 company sign-in only: a user's accounts
//
// Every route takes the chain token (Authorization: Bearer). Changes are
// announced on the change stream as "room" events (package events).
package rooms

import (
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/rs/zerolog/log"

	"github.com/brave/go-sync/auth"
	"github.com/brave/go-sync/datastore"
)

// Limits of the API (the datastore has the room's own).
const (
	MaxWait            = 30 * time.Second // Longest long poll on /changes.
	MaxRecordsPerWrite = 500
	MaxChangesLimit    = 1000
	MaxWriteBytes      = 24 << 20 // A records request's body (base64 inflates by a third).
	MaxBatchCiphertext = 16 << 20 // Ciphertext in one records request.
	maxBody            = 1 << 20  // Any other request's body.
	EnvelopeBytes      = 80       // HPKE enc (32) || sealed room key (32) || tag (16).
	RecordOverhead     = 28       // AES-256-GCM nonce (12) and tag (16) around a record.
	MaxProfileBytes    = 4096
	MaxInviteUses      = 1000
	IdentityKeyBytes   = 32 // X25519 public key.
	SecretBytes        = 32 // An invite's secret; the server keeps its SHA-256.
	RoomIDBytes        = 16
)

// Kinds of room.
var kinds = []string{"space", "folder", "easel"}

var (
	recordIDRE = regexp.MustCompile(`^[A-Za-z0-9._:-]{1,128}$`)
	// A room ID: 16 bytes, base64url without padding (22 characters).
	roomIDRE = regexp.MustCompile(`^[A-Za-z0-9_-]{22}$`)
	// An invite ID: 1 to 64 of base64url's alphabet (the server makes 16
	// random bytes, base64url without padding).
	inviteIDRE   = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)
	secretHashRE = regexp.MustCompile(`^[0-9a-f]{64}$`)
	chainIDRE    = regexp.MustCompile(`^[0-9a-f]{64}$`)
)

// Store is what the handler needs (datastore.SQLite).
type Store interface {
	IsSyncChainDisabled(ctx context.Context, clientID string) (bool, error)
	TouchChain(ctx context.Context, clientID string, now time.Time) error
	ChainBinding(ctx context.Context, clientID string) (*datastore.ChainBinding, error)

	CreateRoom(ctx context.Context, n datastore.NewRoom, now time.Time) (datastore.Room, error)
	GetRoom(ctx context.Context, roomID, caller string) (*datastore.RoomView, error)
	RoomsOf(ctx context.Context, clientID string) ([]datastore.RoomSummary, error)
	IsRoomMember(ctx context.Context, roomID, clientID string) (bool, error)
	WasRoomMember(ctx context.Context, roomID, clientID string) (bool, error)
	HasRooms(ctx context.Context, clientID string) (bool, error)
	DeleteRoom(ctx context.Context, roomID, caller string) (datastore.RoomChange, error)
	CommitRoomRecords(ctx context.Context, roomID, author string, writes []datastore.RoomRecordWrite) (
		[]datastore.RoomRecord, datastore.RoomChange, error)
	RoomChanges(ctx context.Context, roomID, caller string, since int64, limit int) (
		[]datastore.RoomRecord, datastore.Room, bool, error)
	CreateRoomInvite(ctx context.Context, roomID, caller string, inv datastore.RoomInvite) (datastore.RoomChange, error)
	RevokeRoomInvite(ctx context.Context, roomID, caller, inviteID string) (datastore.RoomChange, error)
	CheckRoomInvite(ctx context.Context, roomID, inviteID string, secret []byte, now time.Time) error
	JoinRoom(ctx context.Context, roomID, clientID, inviteID string, secret, identityPub, profile []byte,
		now time.Time) (datastore.RoomChange, bool, error)
	AddRoomMembers(ctx context.Context, roomID, caller string, members []datastore.NewRoomMember,
		now time.Time) (datastore.RoomChange, error)
	SetRoomMemberRole(ctx context.Context, roomID, caller, target, role string) (datastore.RoomChange, error)
	SetRoomProfile(ctx context.Context, roomID, caller string, profile []byte) (datastore.RoomChange, error)
	RemoveRoomMember(ctx context.Context, roomID, caller, target string, now time.Time) (datastore.RoomChange, error)
	TransferRoomOwner(ctx context.Context, roomID, caller, target string) (datastore.RoomChange, error)
	RotateRoomEpoch(ctx context.Context, roomID, caller string, epoch int64, envelopes map[string][]byte) (
		datastore.RoomChange, error)
	AddRoomEnvelopes(ctx context.Context, roomID, caller string, envelopes []datastore.RoomEnvelope) (
		datastore.RoomChange, error)
	SetIdentityKey(ctx context.Context, clientID string, pub []byte, now time.Time) ([]string, error)
	IdentityKey(ctx context.Context, clientID string) ([]byte, error)
	RoomMembersOf(ctx context.Context, roomID string) ([]string, error)
	Directory(ctx context.Context, issuer, email string, now time.Time) ([]datastore.DirectoryEntry, error)
}

// Config wires the handler.
type Config struct {
	Store Store
	// Authorize checks the chain token (auth.AuthorizeRooms, with the
	// company sign-in gate when it's on).
	Authorize auth.RoomAuthorize
	// Notify tells member chains of a change (events.Hub.NotifyRoom).
	// Optional.
	Notify func(chains []string, exceptChain, exceptClient, roomID string, seq int64, change string)
	// OIDCIssuer is the company sign-in's issuer; empty when sign-in is
	// off (no directory, no email invites).
	OIDCIssuer string
	// Now is the clock (tests). Default time.Now.
	Now func() time.Time
	// MaxWait caps long polls (tests shorten it). Default MaxWait.
	MaxWait time.Duration
}

// Handler serves the routes.
type Handler struct {
	cfg Config

	mu      sync.Mutex
	waiters map[string]chan struct{} // Closed at the room's next change.
}

// New returns the handler.
func New(cfg Config) *Handler {
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	if cfg.MaxWait <= 0 {
		cfg.MaxWait = MaxWait
	}
	return &Handler{cfg: cfg, waiters: map[string]chan struct{}{}}
}

// Register adds the routes to |r| (the /v2 router).
func (h *Handler) Register(r chi.Router) {
	r.Get("/compass/rooms", h.list)
	r.Post("/compass/rooms", h.create)
	r.Route("/compass/rooms/{room}", func(r chi.Router) {
		r.Get("/", h.get)
		r.Delete("/", h.deleteRoom)
		r.Get("/changes", h.changes)
		r.Post("/records", h.records)
		r.Post("/invites", h.createInvite)
		r.Delete("/invites/{invite}", h.revokeInvite)
		r.Post("/join", h.join)
		r.Post("/members", h.addMembers)
		r.Patch("/members/{client}", h.updateMember)
		r.Delete("/members/{client}", h.removeMember)
		r.Post("/owner", h.transfer)
		r.Post("/epochs", h.rotate)
		r.Post("/envelopes", h.envelopes)
	})
	r.Get("/compass/identity", h.getIdentity)
	r.Put("/compass/identity", h.putIdentity)
	r.Get("/compass/directory", h.directory)
}

// --- Answers ---

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, status int, code string) {
	writeJSON(w, status, map[string]string{"error": code})
}

func badRequest(w http.ResponseWriter, detail string) {
	writeJSON(w, http.StatusBadRequest, map[string]string{"error": "bad_request", "detail": detail})
}

func noContent(w http.ResponseWriter) {
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusNoContent)
}

// storeError answers an error from the store.
func storeError(w http.ResponseWriter, err error) {
	var conflict *datastore.RoomConflictError
	var mismatch *datastore.RoomEnvelopeError
	switch {
	case errors.As(err, &conflict):
		current := make([]record, 0, len(conflict.Current))
		for _, r := range conflict.Current {
			current = append(current, apiRecord(r))
		}
		writeJSON(w, http.StatusConflict, map[string]any{"error": "conflict", "current": current})
	case errors.As(err, &mismatch):
		writeJSON(w, http.StatusConflict, map[string]any{"error": "envelopes_mismatch",
			"missing": orEmpty(mismatch.Missing), "unknown": orEmpty(mismatch.Unknown)})
	case errors.Is(err, datastore.ErrRoomNotFound):
		fail(w, http.StatusNotFound, "not_found")
	case errors.Is(err, datastore.ErrNotRoomMember):
		fail(w, http.StatusNotFound, "member_not_found")
	case errors.Is(err, datastore.ErrRoomMemberRemoved):
		fail(w, http.StatusForbidden, "removed")
	case errors.Is(err, datastore.ErrRoomForbidden):
		fail(w, http.StatusForbidden, "forbidden")
	case errors.Is(err, datastore.ErrRoomNotJoined):
		fail(w, http.StatusForbidden, "not_joined")
	case errors.Is(err, datastore.ErrInviteInvalid):
		fail(w, http.StatusForbidden, "invite_invalid")
	case errors.Is(err, datastore.ErrInviteExpired):
		fail(w, http.StatusForbidden, "invite_expired")
	case errors.Is(err, datastore.ErrInviteUsedUp):
		fail(w, http.StatusForbidden, "invite_used_up")
	case errors.Is(err, datastore.ErrInviteRevoked):
		fail(w, http.StatusForbidden, "invite_revoked")
	case errors.Is(err, datastore.ErrRoomFull):
		fail(w, http.StatusConflict, "room_full")
	case errors.Is(err, datastore.ErrRoomLimit):
		fail(w, http.StatusConflict, "too_many_rooms")
	case errors.Is(err, datastore.ErrInviteLimit):
		fail(w, http.StatusConflict, "too_many_invites")
	case errors.Is(err, datastore.ErrStaleEpoch):
		fail(w, http.StatusConflict, "stale_epoch")
	case errors.Is(err, datastore.ErrOwnerMustTransfer):
		fail(w, http.StatusConflict, "owner_must_transfer")
	case errors.Is(err, datastore.ErrAlreadyMember):
		fail(w, http.StatusConflict, "already_member")
	case errors.Is(err, datastore.ErrNoIdentityKey):
		fail(w, http.StatusConflict, "no_identity_key")
	case errors.Is(err, datastore.ErrIdentityMismatch):
		fail(w, http.StatusConflict, "identity_mismatch")
	case errors.Is(err, datastore.ErrQuotaExceeded):
		fail(w, http.StatusInsufficientStorage, "quota_exceeded")
	default:
		log.Error().Err(err).Msg("Rooms: request failed")
		fail(w, http.StatusInternalServerError, "unable to complete request")
	}
}

func orEmpty(s []string) []string {
	if s == nil {
		return []string{}
	}
	return s
}

// --- Callers and the gate ---

// caller is an authorized request's chain.
type caller struct {
	id     string // Lower-case.
	sent   string // As the token gave it (the case its sync data is stored in).
	listed bool   // May use the whole server (auth.ChainListed).
	device string // The client_id query parameter (a cache GUID), if any.
}

// authorize checks the token, the sign-in gate and the disabled chain.
func (h *Handler) authorize(w http.ResponseWriter, r *http.Request) *caller {
	id, listed, err := h.cfg.Authorize(r)
	if err != nil || id == "" {
		if !auth.WriteGateError(w, err) {
			fail(w, http.StatusUnauthorized, "invalid sync token")
		}
		return nil
	}
	ctx := r.Context()
	disabled, err := h.cfg.Store.IsSyncChainDisabled(ctx, id)
	if err != nil {
		storeError(w, err)
		return nil
	}
	if disabled {
		fail(w, http.StatusForbidden, "chain_disabled")
		return nil
	}
	if err = h.cfg.Store.TouchChain(ctx, id, h.cfg.Now()); err != nil {
		log.Warn().Err(err).Msg("Rooms: touching the chain failed")
	}
	return &caller{id: strings.ToLower(id), sent: id, listed: listed, device: r.URL.Query().Get("client_id")}
}

// notAllowed answers a chain the private server doesn't list, on a route
// it may not use.
func notAllowed(w http.ResponseWriter) {
	fail(w, http.StatusForbidden, "chain_not_allowed")
}

// full authorizes a route only listed chains may use (creating a room,
// the directory).
func (h *Handler) full(w http.ResponseWriter, r *http.Request) *caller {
	c := h.authorize(w, r)
	if c != nil && !c.listed {
		notAllowed(w)
		return nil
	}
	return c
}

// anyRoom authorizes a route a chain the private server doesn't list may
// use while it's a member of a room (its rooms, its identity key).
func (h *Handler) anyRoom(w http.ResponseWriter, r *http.Request) *caller {
	c := h.authorize(w, r)
	if c == nil || c.listed {
		return c
	}
	ok, err := h.cfg.Store.HasRooms(r.Context(), c.id)
	if err != nil {
		storeError(w, err)
		return nil
	}
	if !ok {
		notAllowed(w)
		return nil
	}
	return c
}

// member authorizes a route of room {room}; a chain the private server
// doesn't list must be, or have been, a member of it (a removed member is
// then told it was removed).
func (h *Handler) member(w http.ResponseWriter, r *http.Request) (*caller, string) {
	roomID := chi.URLParam(r, "room")
	c := h.authorize(w, r)
	if c == nil {
		return nil, ""
	}
	if !validRoomID(roomID) {
		fail(w, http.StatusNotFound, "not_found")
		return nil, ""
	}
	if !c.listed {
		ok, err := h.cfg.Store.WasRoomMember(r.Context(), roomID, c.id)
		if err != nil {
			storeError(w, err)
			return nil, ""
		}
		if !ok {
			notAllowed(w)
			return nil, ""
		}
	}
	return c, roomID
}

// decode reads a JSON body of at most |limit| bytes into |v|.
func decode(w http.ResponseWriter, r *http.Request, limit int64, v any) bool {
	body := http.MaxBytesReader(w, r.Body, limit)
	dec := json.NewDecoder(body)
	if err := dec.Decode(v); err != nil {
		var tooBig *http.MaxBytesError
		if errors.As(err, &tooBig) {
			fail(w, http.StatusRequestEntityTooLarge, "too_large")
			return false
		}
		badRequest(w, "invalid JSON: "+err.Error())
		return false
	}
	if _, err := dec.Token(); err != io.EOF {
		badRequest(w, "trailing data after the JSON object")
		return false
	}
	return true
}

// --- Notifications and long polls ---

// changed tells the members of |ch| and wakes the room's long polls.
func (h *Handler) changed(c *caller, roomID string, ch datastore.RoomChange, change string) {
	h.mu.Lock()
	if w, ok := h.waiters[roomID]; ok {
		close(w)
		delete(h.waiters, roomID)
	}
	h.mu.Unlock()
	if h.cfg.Notify != nil {
		h.cfg.Notify(ch.Members, c.id, c.device, roomID, ch.Seq, change)
	}
}

// waiter returns a channel closed at room |roomID|'s next change.
func (h *Handler) waiter(roomID string) <-chan struct{} {
	h.mu.Lock()
	defer h.mu.Unlock()
	w, ok := h.waiters[roomID]
	if !ok {
		w = make(chan struct{})
		h.waiters[roomID] = w
	}
	return w
}

// --- JSON shapes ---

type member struct {
	ClientID    string `json:"client_id"`
	Role        string `json:"role"`
	IdentityPub []byte `json:"identity_pub"`
	Profile     []byte `json:"profile"`
	Name        string `json:"name"`
	Email       string `json:"email"`
	Invited     int64  `json:"invited"`
	Joined      int64  `json:"joined"`
	Pending     bool   `json:"pending"`
}

type envelope struct {
	ClientID string `json:"client_id,omitempty"`
	Epoch    int64  `json:"epoch"`
	Sealed   []byte `json:"sealed"`
}

type invite struct {
	ID       string `json:"id"`
	Role     string `json:"role"`
	Created  int64  `json:"created"`
	Expires  int64  `json:"expires"`
	UsesLeft *int64 `json:"uses_left"`
	Revoked  bool   `json:"revoked"`
}

type record struct {
	ID         string `json:"id"`
	Seq        int64  `json:"seq"`
	Epoch      int64  `json:"epoch"`
	Deleted    bool   `json:"deleted"`
	Author     string `json:"author"`
	Ciphertext []byte `json:"ciphertext,omitempty"`
}

type room struct {
	ID        string     `json:"id"`
	Kind      string     `json:"kind"`
	Owner     string     `json:"owner"`
	Created   int64      `json:"created"`
	Epoch     int64      `json:"epoch"`
	Seq       int64      `json:"seq"`
	Bytes     int64      `json:"bytes"`
	Role      string     `json:"role"`
	Pending   bool       `json:"pending"`
	Members   []member   `json:"members"`
	Envelopes []envelope `json:"envelopes"`
	Invites   []invite   `json:"invites,omitempty"`
}

func msOf(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.UnixMilli()
}

func apiRecord(r datastore.RoomRecord) record {
	return record{ID: r.ID, Seq: r.Seq, Epoch: r.Epoch, Deleted: r.Deleted, Author: r.Author, Ciphertext: r.Ciphertext}
}

func apiInvite(inv datastore.RoomInvite) invite {
	return invite{ID: inv.ID, Role: inv.Role, Created: msOf(inv.Created), Expires: msOf(inv.Expires),
		UsesLeft: inv.UsesLeft, Revoked: inv.Revoked}
}

// view answers room |roomID| as |c| sees it.
func (h *Handler) view(ctx context.Context, w http.ResponseWriter, c *caller, roomID string, status int) {
	v, err := h.cfg.Store.GetRoom(ctx, roomID, c.id)
	if err != nil {
		storeError(w, err)
		return
	}
	out := room{ID: v.Room.ID, Kind: v.Room.Kind, Owner: v.Room.Owner, Created: msOf(v.Room.Created),
		Epoch: v.Room.Epoch, Seq: v.Room.Seq, Bytes: v.Room.Bytes, Role: v.Me.Role, Pending: v.Me.Pending(),
		Members: []member{}, Envelopes: []envelope{}}
	for _, m := range v.Members {
		am := member{ClientID: m.ClientID, Role: m.Role, IdentityPub: m.IdentityPub, Profile: m.Profile,
			Invited: msOf(m.Invited), Joined: msOf(m.Joined), Pending: m.Pending()}
		if h.cfg.OIDCIssuer != "" {
			b, err := h.cfg.Store.ChainBinding(ctx, m.ClientID)
			if err != nil {
				storeError(w, err)
				return
			}
			if b != nil && b.User.Issuer == h.cfg.OIDCIssuer {
				am.Name, am.Email = b.User.Name, b.User.Email
			}
		}
		out.Members = append(out.Members, am)
	}
	for _, e := range v.Envelopes {
		out.Envelopes = append(out.Envelopes, envelope{Epoch: e.Epoch, Sealed: e.Sealed})
	}
	if v.Me.Role == datastore.RoleOwner {
		out.Invites = []invite{}
		for _, inv := range v.Invites {
			out.Invites = append(out.Invites, apiInvite(inv))
		}
	}
	writeJSON(w, status, out)
}

func newID() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	return base64.RawURLEncoding.EncodeToString(b)
}

func validRole(role string) bool { return role == datastore.RoleEditor || role == datastore.RoleViewer }

func validKind(kind string) bool {
	for _, k := range kinds {
		if k == kind {
			return true
		}
	}
	return false
}

func validEnvelope(b []byte) bool { return len(b) == EnvelopeBytes }

// validRoomID says whether |id| is canonical unpadded base64url of 16
// bytes.
func validRoomID(id string) bool {
	if !roomIDRE.MatchString(id) {
		return false
	}
	b, err := base64.RawURLEncoding.Strict().DecodeString(id)
	return err == nil && len(b) == RoomIDBytes
}

// decodeSecret decodes an invite secret: unpadded base64url of 32 bytes
// (as the link's "x"), or nil.
func decodeSecret(s string) []byte {
	b, err := base64.RawURLEncoding.Strict().DecodeString(s)
	if err != nil || len(b) != SecretBytes {
		return nil
	}
	return b
}

// --- Routes ---

func (h *Handler) list(w http.ResponseWriter, r *http.Request) {
	c := h.anyRoom(w, r)
	if c == nil {
		return
	}
	rooms, err := h.cfg.Store.RoomsOf(r.Context(), c.id)
	if err != nil {
		storeError(w, err)
		return
	}
	type summary struct {
		ID      string `json:"id"`
		Kind    string `json:"kind"`
		Owner   string `json:"owner"`
		Role    string `json:"role"`
		Pending bool   `json:"pending"`
		Epoch   int64  `json:"epoch"`
		Seq     int64  `json:"seq"`
	}
	out := []summary{}
	for _, s := range rooms {
		out = append(out, summary{ID: s.Room.ID, Kind: s.Room.Kind, Owner: s.Room.Owner, Role: s.Me.Role,
			Pending: s.Me.Pending(), Epoch: s.Room.Epoch, Seq: s.Room.Seq})
	}
	writeJSON(w, http.StatusOK, map[string]any{"rooms": out})
}

func (h *Handler) create(w http.ResponseWriter, r *http.Request) {
	c := h.full(w, r)
	if c == nil {
		return
	}
	var req struct {
		Kind        string `json:"kind"`
		IdentityPub []byte `json:"identity_pub"`
		Envelope    []byte `json:"envelope"`
		Profile     []byte `json:"profile"`
	}
	if !decode(w, r, maxBody, &req) {
		return
	}
	switch {
	case !validKind(req.Kind):
		badRequest(w, "kind must be space, folder or easel")
		return
	case len(req.IdentityPub) != IdentityKeyBytes:
		badRequest(w, "identity_pub must be 32 bytes")
		return
	case !validEnvelope(req.Envelope):
		badRequest(w, "envelope must be 80 bytes")
		return
	case len(req.Profile) > MaxProfileBytes:
		badRequest(w, "profile must be at most 4096 bytes")
		return
	}
	ctx := r.Context()
	rm, err := h.cfg.Store.CreateRoom(ctx, datastore.NewRoom{ID: newID(), Owner: c.sent, Kind: req.Kind,
		IdentityPub: req.IdentityPub, Profile: req.Profile, Envelope: req.Envelope}, h.cfg.Now())
	if err != nil {
		storeError(w, err)
		return
	}
	log.Info().Str("room", rm.ID).Str("kind", rm.Kind).Str("chainID", c.id).Msg("Room created")
	h.view(ctx, w, c, rm.ID, http.StatusCreated)
}

func (h *Handler) get(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	h.view(r.Context(), w, c, roomID, http.StatusOK)
}

func (h *Handler) deleteRoom(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	ch, err := h.cfg.Store.DeleteRoom(r.Context(), roomID, c.id)
	if err != nil {
		storeError(w, err)
		return
	}
	log.Info().Str("room", roomID).Msg("Room deleted by its owner")
	h.changed(c, roomID, ch, "deleted")
	noContent(w)
}

func (h *Handler) changes(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	q := r.URL.Query()
	since, limit, wait := int64(0), MaxChangesLimit, time.Duration(0)
	var err error
	if v := q.Get("since"); v != "" {
		if since, err = strconv.ParseInt(v, 10, 64); err != nil || since < 0 {
			badRequest(w, "since must be a sequence number")
			return
		}
	}
	if v := q.Get("limit"); v != "" {
		if limit, err = strconv.Atoi(v); err != nil || limit < 1 || limit > MaxChangesLimit {
			badRequest(w, "limit must be 1 to 1000")
			return
		}
	}
	if v := q.Get("wait"); v != "" {
		secs, err := strconv.Atoi(v)
		if err != nil || secs < 0 || secs > int(MaxWait/time.Second) {
			badRequest(w, "wait must be 0 to 30 seconds")
			return
		}
		wait = min(time.Duration(secs)*time.Second, h.cfg.MaxWait)
	}
	ctx := r.Context()
	deadline := time.NewTimer(wait)
	defer deadline.Stop()
	// The first read checks access; only then is a waiter registered (so a
	// stranger can't make waiters for made-up rooms), and each later read
	// happens after registering, so no change slips in between.
	registered := false
	for {
		var woken <-chan struct{}
		if registered {
			woken = h.waiter(roomID)
		}
		records, rm, more, err := h.cfg.Store.RoomChanges(ctx, roomID, c.id, since, limit)
		if err != nil {
			storeError(w, err)
			return
		}
		if len(records) > 0 || wait == 0 {
			out := make([]record, 0, len(records))
			for _, rec := range records {
				out = append(out, apiRecord(rec))
			}
			writeJSON(w, http.StatusOK, map[string]any{"seq": rm.Seq, "epoch": rm.Epoch, "records": out,
				"more": more})
			return
		}
		if !registered {
			registered = true
			continue
		}
		select {
		case <-woken:
			continue
		case <-deadline.C:
			wait = 0 // Answer what there is (nothing).
		case <-ctx.Done():
			return
		}
	}
}

func (h *Handler) records(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	var req struct {
		Records []struct {
			ID         string `json:"id"`
			Epoch      int64  `json:"epoch"`
			Deleted    bool   `json:"deleted"`
			Ciphertext []byte `json:"ciphertext"`
			BaseSeq    *int64 `json:"base_seq"`
		} `json:"records"`
	}
	if !decode(w, r, MaxWriteBytes, &req) {
		return
	}
	if len(req.Records) == 0 || len(req.Records) > MaxRecordsPerWrite {
		badRequest(w, "records must hold 1 to 500 records")
		return
	}
	writes := make([]datastore.RoomRecordWrite, 0, len(req.Records))
	seen := map[string]bool{}
	total := 0
	for _, rec := range req.Records {
		switch {
		case !recordIDRE.MatchString(rec.ID):
			badRequest(w, "record id must be 1 to 128 of A-Z a-z 0-9 . _ : -")
			return
		case seen[rec.ID]:
			badRequest(w, "record "+rec.ID+" appears twice")
			return
		case rec.Epoch < 1:
			badRequest(w, "epoch must be 1 or more")
			return
		case !rec.Deleted && len(rec.Ciphertext) < RecordOverhead:
			badRequest(w, "record "+rec.ID+" needs a ciphertext of 28 bytes or more (nonce, ciphertext, tag)")
			return
		case len(rec.Ciphertext) > datastore.MaxRoomRecordBytes:
			fail(w, http.StatusRequestEntityTooLarge, "record_too_large")
			return
		case rec.BaseSeq != nil && *rec.BaseSeq < 0:
			badRequest(w, "base_seq must be 0 or more")
			return
		}
		seen[rec.ID] = true
		total += len(rec.Ciphertext)
		writes = append(writes, datastore.RoomRecordWrite{ID: rec.ID, Epoch: rec.Epoch, Deleted: rec.Deleted,
			Ciphertext: rec.Ciphertext, BaseSeq: rec.BaseSeq})
	}
	if total > MaxBatchCiphertext {
		fail(w, http.StatusRequestEntityTooLarge, "too_large")
		return
	}
	stored, ch, err := h.cfg.Store.CommitRoomRecords(r.Context(), roomID, c.id, writes)
	if err != nil {
		storeError(w, err)
		return
	}
	type written struct {
		ID  string `json:"id"`
		Seq int64  `json:"seq"`
	}
	out := make([]written, 0, len(stored))
	for _, s := range stored {
		out = append(out, written{ID: s.ID, Seq: s.Seq})
	}
	h.changed(c, roomID, ch, "records")
	writeJSON(w, http.StatusOK, map[string]any{"seq": ch.Seq, "records": out})
}

func (h *Handler) createInvite(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	var req struct {
		Role       string `json:"role"`
		SecretHash string `json:"secret_hash"`
		Expires    int64  `json:"expires"`
		Uses       int64  `json:"uses"`
	}
	if !decode(w, r, maxBody, &req) {
		return
	}
	now := h.cfg.Now()
	switch {
	case !validRole(req.Role):
		badRequest(w, "role must be editor or viewer")
		return
	case !secretHashRE.MatchString(req.SecretHash):
		badRequest(w, "secret_hash must be the SHA-256 of the secret, 64 lower-case hex digits")
		return
	case req.Expires < 0 || (req.Expires != 0 && req.Expires <= now.UnixMilli()):
		badRequest(w, "expires must be 0 or in the future")
		return
	case req.Uses < 0 || req.Uses > MaxInviteUses:
		badRequest(w, "uses must be 0 to 1000")
		return
	}
	hash, _ := hex.DecodeString(req.SecretHash)
	inv := datastore.RoomInvite{ID: newID(), SecretHash: hash, Role: req.Role, Created: now}
	if req.Expires != 0 {
		inv.Expires = time.UnixMilli(req.Expires)
	}
	if req.Uses != 0 {
		inv.UsesLeft = &req.Uses
	}
	ch, err := h.cfg.Store.CreateRoomInvite(r.Context(), roomID, c.id, inv)
	if err != nil {
		storeError(w, err)
		return
	}
	h.changed(c, roomID, ch, "invites")
	writeJSON(w, http.StatusCreated, apiInvite(inv))
}

func (h *Handler) revokeInvite(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	inviteID := chi.URLParam(r, "invite")
	if !inviteIDRE.MatchString(inviteID) {
		fail(w, http.StatusNotFound, "invite_not_found")
		return
	}
	ch, err := h.cfg.Store.RevokeRoomInvite(r.Context(), roomID, c.id, inviteID)
	if errors.Is(err, datastore.ErrInviteInvalid) {
		fail(w, http.StatusNotFound, "invite_not_found")
		return
	}
	if err != nil {
		storeError(w, err)
		return
	}
	h.changed(c, roomID, ch, "invites")
	noContent(w)
}

func (h *Handler) join(w http.ResponseWriter, r *http.Request) {
	roomID := chi.URLParam(r, "room")
	c := h.authorize(w, r)
	if c == nil {
		return
	}
	var req struct {
		InviteID    string `json:"invite_id"`
		Secret      string `json:"secret"`
		IdentityPub []byte `json:"identity_pub"`
		Profile     []byte `json:"profile"`
	}
	if !decode(w, r, maxBody, &req) {
		return
	}
	switch {
	case !validRoomID(roomID):
		fail(w, http.StatusNotFound, "not_found")
		return
	case len(req.IdentityPub) != IdentityKeyBytes:
		badRequest(w, "identity_pub must be 32 bytes")
		return
	case len(req.Profile) > MaxProfileBytes:
		badRequest(w, "profile must be at most 4096 bytes")
		return
	case req.InviteID != "" && (!inviteIDRE.MatchString(req.InviteID) || decodeSecret(req.Secret) == nil):
		fail(w, http.StatusForbidden, "invite_invalid")
		return
	}
	ctx := r.Context()
	now := h.cfg.Now()
	secret := decodeSecret(req.Secret)
	if !c.listed {
		// The gate: a pending (or former) member, or a valid invite.
		ok, err := h.cfg.Store.WasRoomMember(ctx, roomID, c.id)
		if err != nil {
			storeError(w, err)
			return
		}
		if !ok {
			if req.InviteID == "" {
				notAllowed(w)
				return
			}
			if err = h.cfg.Store.CheckRoomInvite(ctx, roomID, req.InviteID, secret, now); err != nil {
				if errors.Is(err, datastore.ErrInviteInvalid) {
					notAllowed(w)
				} else {
					storeError(w, err)
				}
				return
			}
		}
	}
	ch, joined, err := h.cfg.Store.JoinRoom(ctx, roomID, c.id, req.InviteID, secret, req.IdentityPub,
		req.Profile, now)
	if errors.Is(err, datastore.ErrRoomNotFound) {
		// Without an invite that opens it, a room is never shown to exist.
		fail(w, http.StatusForbidden, "invite_invalid")
		return
	}
	if err != nil {
		storeError(w, err)
		return
	}
	if joined {
		log.Info().Str("room", roomID).Str("chainID", c.id).Msg("Room joined")
		h.changed(c, roomID, ch, "members")
	}
	h.view(ctx, w, c, roomID, http.StatusOK)
}

func (h *Handler) addMembers(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	if h.cfg.OIDCIssuer == "" {
		fail(w, http.StatusNotFound, "directory_disabled")
		return
	}
	var req struct {
		Members []struct {
			ClientID    string     `json:"client_id"`
			Role        string     `json:"role"`
			IdentityPub []byte     `json:"identity_pub"`
			Envelopes   []envelope `json:"envelopes"`
		} `json:"members"`
	}
	if !decode(w, r, maxBody, &req) {
		return
	}
	if len(req.Members) == 0 || len(req.Members) > datastore.MaxRoomMembers {
		badRequest(w, "members must hold 1 to 50 members")
		return
	}
	ctx := r.Context()
	now := h.cfg.Now()
	members := make([]datastore.NewRoomMember, 0, len(req.Members))
	seen := map[string]bool{}
	for _, m := range req.Members {
		id := strings.ToLower(m.ClientID)
		switch {
		case !chainIDRE.MatchString(id):
			badRequest(w, "client_id must be 64 hex digits")
			return
		case seen[id]:
			badRequest(w, "member "+id+" appears twice")
			return
		case !validRole(m.Role):
			badRequest(w, "role must be editor or viewer")
			return
		case len(m.IdentityPub) != IdentityKeyBytes:
			badRequest(w, "identity_pub must be 32 bytes")
			return
		case len(m.Envelopes) == 0:
			badRequest(w, "each member needs envelopes")
			return
		}
		seen[id] = true
		nm := datastore.NewRoomMember{ClientID: id, Role: m.Role, IdentityPub: m.IdentityPub}
		for _, e := range m.Envelopes {
			if !validEnvelope(e.Sealed) {
				badRequest(w, "sealed must be 80 bytes")
				return
			}
			nm.Envelopes = append(nm.Envelopes, datastore.RoomEnvelope{ClientID: id, Epoch: e.Epoch, Sealed: e.Sealed})
		}
		// Email invites reach signed-in accounts only.
		b, err := h.cfg.Store.ChainBinding(ctx, id)
		if err != nil {
			storeError(w, err)
			return
		}
		if !b.Valid(h.cfg.OIDCIssuer, now) {
			writeJSON(w, http.StatusConflict, map[string]string{"error": "not_signed_in", "client_id": id})
			return
		}
		members = append(members, nm)
	}
	ch, err := h.cfg.Store.AddRoomMembers(ctx, roomID, c.id, members, now)
	if err != nil {
		storeError(w, err)
		return
	}
	h.changed(c, roomID, ch, "members")
	h.view(ctx, w, c, roomID, http.StatusOK)
}

// target is the {client} parameter, lower-case, or "" after a 404.
func target(w http.ResponseWriter, r *http.Request) string {
	id := strings.ToLower(chi.URLParam(r, "client"))
	if !chainIDRE.MatchString(id) {
		fail(w, http.StatusNotFound, "member_not_found")
		return ""
	}
	return id
}

func (h *Handler) updateMember(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	who := target(w, r)
	if who == "" {
		return
	}
	var fields map[string]json.RawMessage
	if !decode(w, r, maxBody, &fields) {
		return
	}
	var role *string
	var profile []byte
	rawRole, hasRole := fields["role"]
	rawProfile, hasProfile := fields["profile"]
	for k := range fields {
		if k != "role" && k != "profile" {
			badRequest(w, "unknown field "+k)
			return
		}
	}
	if !hasRole && !hasProfile {
		badRequest(w, "nothing to change")
		return
	}
	if hasRole {
		if err := json.Unmarshal(rawRole, &role); err != nil || role == nil || !validRole(*role) {
			badRequest(w, "role must be editor or viewer")
			return
		}
	}
	if hasProfile {
		if err := json.Unmarshal(rawProfile, &profile); err != nil {
			badRequest(w, "profile must be base64 or null")
			return
		}
		if len(profile) > MaxProfileBytes {
			badRequest(w, "profile must be at most 4096 bytes")
			return
		}
		if who != c.id {
			fail(w, http.StatusForbidden, "forbidden")
			return
		}
	}
	ctx := r.Context()
	if role != nil {
		ch, err := h.cfg.Store.SetRoomMemberRole(ctx, roomID, c.id, who, *role)
		if err != nil {
			storeError(w, err)
			return
		}
		h.changed(c, roomID, ch, "members")
	}
	if hasProfile {
		ch, err := h.cfg.Store.SetRoomProfile(ctx, roomID, c.id, profile)
		if err != nil {
			storeError(w, err)
			return
		}
		h.changed(c, roomID, ch, "members")
	}
	noContent(w)
}

func (h *Handler) removeMember(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	who := target(w, r)
	if who == "" {
		return
	}
	ch, err := h.cfg.Store.RemoveRoomMember(r.Context(), roomID, c.id, who, h.cfg.Now())
	if err != nil {
		storeError(w, err)
		return
	}
	log.Info().Str("room", roomID).Str("chainID", who).Bool("left", who == c.id).Msg("Room member removed")
	h.changed(c, roomID, ch, "members")
	noContent(w)
}

func (h *Handler) transfer(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	var req struct {
		ClientID string `json:"client_id"`
	}
	if !decode(w, r, maxBody, &req) {
		return
	}
	id := strings.ToLower(req.ClientID)
	if !chainIDRE.MatchString(id) {
		badRequest(w, "client_id must be 64 hex digits")
		return
	}
	ch, err := h.cfg.Store.TransferRoomOwner(r.Context(), roomID, c.id, id)
	if err != nil {
		storeError(w, err)
		return
	}
	if len(ch.Members) > 0 {
		h.changed(c, roomID, ch, "members")
	}
	noContent(w)
}

func (h *Handler) rotate(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	var req struct {
		Epoch     int64      `json:"epoch"`
		Envelopes []envelope `json:"envelopes"`
	}
	if !decode(w, r, maxBody, &req) {
		return
	}
	if req.Epoch < 2 {
		badRequest(w, "epoch must be 2 or more")
		return
	}
	byID := map[string][]byte{}
	for _, e := range req.Envelopes {
		id := strings.ToLower(e.ClientID)
		switch {
		case !chainIDRE.MatchString(id):
			badRequest(w, "client_id must be 64 hex digits")
			return
		case byID[id] != nil:
			badRequest(w, "two envelopes for "+id)
			return
		case !validEnvelope(e.Sealed):
			badRequest(w, "sealed must be 80 bytes")
			return
		}
		byID[id] = e.Sealed
	}
	ch, err := h.cfg.Store.RotateRoomEpoch(r.Context(), roomID, c.id, req.Epoch, byID)
	if err != nil {
		storeError(w, err)
		return
	}
	log.Info().Str("room", roomID).Int64("epoch", req.Epoch).Msg("Room key rotated")
	h.changed(c, roomID, ch, "keys")
	noContent(w)
}

func (h *Handler) envelopes(w http.ResponseWriter, r *http.Request) {
	c, roomID := h.member(w, r)
	if c == nil {
		return
	}
	var req struct {
		Envelopes []envelope `json:"envelopes"`
	}
	if !decode(w, r, maxBody, &req) {
		return
	}
	if len(req.Envelopes) == 0 || len(req.Envelopes) > 1000 {
		badRequest(w, "envelopes must hold 1 to 1000 envelopes")
		return
	}
	out := make([]datastore.RoomEnvelope, 0, len(req.Envelopes))
	for _, e := range req.Envelopes {
		id := strings.ToLower(e.ClientID)
		if !chainIDRE.MatchString(id) {
			badRequest(w, "client_id must be 64 hex digits")
			return
		}
		if !validEnvelope(e.Sealed) {
			badRequest(w, "sealed must be 80 bytes")
			return
		}
		out = append(out, datastore.RoomEnvelope{ClientID: id, Epoch: e.Epoch, Sealed: e.Sealed})
	}
	ch, err := h.cfg.Store.AddRoomEnvelopes(r.Context(), roomID, c.id, out)
	if err != nil {
		storeError(w, err)
		return
	}
	h.changed(c, roomID, ch, "keys")
	noContent(w)
}

func (h *Handler) getIdentity(w http.ResponseWriter, r *http.Request) {
	c := h.anyRoom(w, r)
	if c == nil {
		return
	}
	pub, err := h.cfg.Store.IdentityKey(r.Context(), c.id)
	if err != nil {
		storeError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"identity_pub": pub})
}

func (h *Handler) putIdentity(w http.ResponseWriter, r *http.Request) {
	c := h.anyRoom(w, r)
	if c == nil {
		return
	}
	var req struct {
		IdentityPub []byte `json:"identity_pub"`
	}
	if !decode(w, r, maxBody, &req) {
		return
	}
	if len(req.IdentityPub) != IdentityKeyBytes {
		badRequest(w, "identity_pub must be 32 bytes")
		return
	}
	ctx := r.Context()
	rooms, err := h.cfg.Store.SetIdentityKey(ctx, c.id, req.IdentityPub, h.cfg.Now())
	if err != nil {
		storeError(w, err)
		return
	}
	for _, roomID := range rooms {
		members, err := h.cfg.Store.RoomMembersOf(ctx, roomID)
		if err != nil {
			log.Warn().Err(err).Msg("Rooms: reading members to notify failed")
			continue
		}
		h.changed(c, roomID, datastore.RoomChange{Members: members}, "members")
	}
	noContent(w)
}

func (h *Handler) directory(w http.ResponseWriter, r *http.Request) {
	c := h.full(w, r)
	if c == nil {
		return
	}
	if h.cfg.OIDCIssuer == "" {
		fail(w, http.StatusNotFound, "directory_disabled")
		return
	}
	email := strings.TrimSpace(r.URL.Query().Get("email"))
	if email == "" || len(email) > 320 || !strings.Contains(email, "@") {
		badRequest(w, "email is required")
		return
	}
	entries, err := h.cfg.Store.Directory(r.Context(), h.cfg.OIDCIssuer, email, h.cfg.Now())
	if err != nil {
		storeError(w, err)
		return
	}
	type account struct {
		ClientID    string `json:"client_id"`
		IdentityPub []byte `json:"identity_pub"`
		Name        string `json:"name"`
		Email       string `json:"email"`
	}
	out := []account{}
	for _, e := range entries {
		out = append(out, account{ClientID: e.ClientID, IdentityPub: e.IdentityPub, Name: e.Name, Email: e.Email})
	}
	writeJSON(w, http.StatusOK, map[string]any{"accounts": out})
}
