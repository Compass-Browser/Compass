package datastore

// Compass: rooms, the shared spaces, folders and easels between users
// (docs/design/sync.md, "Rooms"). A room is a set of encrypted records,
// each with a server-assigned sequence number, and its members: chain
// accounts with a role (owner, editor, viewer). The server never sees a
// room key; members get it as envelopes sealed to their identity keys.
// Chain IDs are stored lower-case. Every rule that depends on the room's
// state (roles, epochs, limits) is checked inside the writing transaction.

import (
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"errors"
	"fmt"
	"sort"
	"strings"
	"time"
)

const roomsSchema = `
CREATE TABLE IF NOT EXISTS rooms (
	id              TEXT PRIMARY KEY,
	owner_client_id TEXT NOT NULL,
	quota_chain     TEXT NOT NULL,
	kind            TEXT NOT NULL,
	created         INTEGER NOT NULL,
	seq             INTEGER NOT NULL DEFAULT 0,
	epoch           INTEGER NOT NULL DEFAULT 1,
	bytes           INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS rooms_by_owner ON rooms (owner_client_id);
CREATE TABLE IF NOT EXISTS room_members (
	room_id      TEXT NOT NULL REFERENCES rooms (id) ON DELETE CASCADE,
	client_id    TEXT NOT NULL,
	role         TEXT NOT NULL,
	identity_pub BLOB,
	profile      BLOB,
	invited      INTEGER NOT NULL,
	joined       INTEGER NOT NULL DEFAULT 0,
	removed      INTEGER NOT NULL DEFAULT 0,
	PRIMARY KEY (room_id, client_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS room_members_by_client ON room_members (client_id);
CREATE TABLE IF NOT EXISTS room_records (
	room_id    TEXT NOT NULL REFERENCES rooms (id) ON DELETE CASCADE,
	record_id  TEXT NOT NULL,
	seq        INTEGER NOT NULL,
	epoch      INTEGER NOT NULL,
	deleted    INTEGER NOT NULL DEFAULT 0,
	author     TEXT NOT NULL,
	ciphertext BLOB,
	size       INTEGER NOT NULL DEFAULT 0,
	PRIMARY KEY (room_id, record_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS room_records_by_seq ON room_records (room_id, seq);
CREATE TABLE IF NOT EXISTS room_envelopes (
	room_id   TEXT NOT NULL REFERENCES rooms (id) ON DELETE CASCADE,
	client_id TEXT NOT NULL,
	epoch     INTEGER NOT NULL,
	sealed    BLOB NOT NULL,
	PRIMARY KEY (room_id, client_id, epoch)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS room_invites (
	room_id     TEXT NOT NULL REFERENCES rooms (id) ON DELETE CASCADE,
	id          TEXT NOT NULL,
	secret_hash BLOB NOT NULL,
	role        TEXT NOT NULL,
	created     INTEGER NOT NULL,
	expires     INTEGER NOT NULL DEFAULT 0,
	uses_left   INTEGER,
	revoked     INTEGER NOT NULL DEFAULT 0,
	PRIMARY KEY (room_id, id)
) WITHOUT ROWID;
CREATE TABLE IF NOT EXISTS identity_keys (
	client_id TEXT PRIMARY KEY,
	pub       BLOB NOT NULL,
	updated   INTEGER NOT NULL
) WITHOUT ROWID;
`

// Room roles.
const (
	RoleOwner  = "owner"
	RoleEditor = "editor"
	RoleViewer = "viewer"
)

// Room limits.
const (
	MaxRoomMembers      = 50      // Members (joined and pending) per room.
	MaxRoomRecordBytes  = 1 << 20 // Ciphertext per record.
	MaxRoomInvites      = 100     // Invites per room that aren't revoked.
	MaxRoomsPerOwner    = 1000    // Rooms a chain owns.
	maxRoomChangesBytes = 8 << 20 // Ciphertext per changes page (at least one record).
)

// Room errors. The rooms handler maps each to an HTTP answer.
var (
	ErrRoomNotFound      = errors.New("room not found")          // Or the caller was never a member.
	ErrRoomMemberRemoved = errors.New("removed from the room")   // The caller was a member.
	ErrRoomForbidden     = errors.New("role doesn't allow it")   // A member, but not this.
	ErrRoomNotJoined     = errors.New("invitation not accepted") // A pending member.
	ErrRoomFull          = errors.New("room is full")
	ErrRoomLimit         = errors.New("too many rooms")
	ErrRoomExists        = errors.New("room ID taken")
	ErrInviteInvalid     = errors.New("invalid invite") // Unknown, or the wrong secret.
	ErrInviteExpired     = errors.New("invite expired")
	ErrInviteUsedUp      = errors.New("invite used up")
	ErrInviteRevoked     = errors.New("invite revoked")
	ErrInviteLimit       = errors.New("too many invites")
	ErrStaleEpoch        = errors.New("stale epoch")
	ErrOwnerMustTransfer = errors.New("the owner can't leave")
	ErrAlreadyMember     = errors.New("already a member")
	ErrNoIdentityKey     = errors.New("no identity key")
	ErrIdentityMismatch  = errors.New("identity key changed")
	ErrNotRoomMember     = errors.New("not a member") // The target of a member operation.
)

// RoomConflictError is a records commit refused because a record's
// base_seq didn't match: Current holds the records as stored (Seq 0: the
// record doesn't exist).
type RoomConflictError struct{ Current []RoomRecord }

func (e *RoomConflictError) Error() string { return "record conflict" }

// RoomEnvelopeError is a rotation whose envelopes don't match the
// members: Missing members got none, Unknown aren't members.
type RoomEnvelopeError struct{ Missing, Unknown []string }

func (e *RoomEnvelopeError) Error() string { return "envelopes don't match the members" }

// Room is one room's row.
type Room struct {
	ID         string
	Owner      string // Lower-case chain ID.
	QuotaChain string // The chains row its bytes count against (as stored).
	Kind       string
	Created    time.Time
	Seq        int64 // The latest record's sequence number (0: none yet).
	Epoch      int64
	Bytes      int64
	Members    int // Joined and pending (ListRooms only).
}

// RoomMember is one membership.
type RoomMember struct {
	ClientID    string
	Role        string
	IdentityPub []byte
	Profile     []byte
	Invited     time.Time
	Joined      time.Time // Zero: pending (an email invite not accepted yet).
	Removed     time.Time // Zero: still a member.
}

// Pending says whether the member was invited by email and hasn't joined.
func (m *RoomMember) Pending() bool { return m.Joined.IsZero() }

// RoomRecord is one record as stored.
type RoomRecord struct {
	ID         string
	Seq        int64
	Epoch      int64
	Deleted    bool
	Author     string
	Ciphertext []byte
}

// RoomRecordWrite is one record of a commit. BaseSeq, if set, must be the
// stored record's Seq (0: the record must not exist).
type RoomRecordWrite struct {
	ID         string
	Epoch      int64
	Deleted    bool
	Ciphertext []byte
	BaseSeq    *int64
}

// RoomEnvelope is a room key of one epoch sealed to one member.
type RoomEnvelope struct {
	ClientID string
	Epoch    int64
	Sealed   []byte
}

// RoomInvite is a link invite. The secret stays with the clients; the
// server keeps its SHA-256.
type RoomInvite struct {
	ID         string
	SecretHash []byte
	Role       string
	Created    time.Time
	Expires    time.Time // Zero: never.
	UsesLeft   *int64    // Nil: unlimited.
	Revoked    bool
}

// RoomView is what a member sees of a room.
type RoomView struct {
	Room      Room
	Me        RoomMember
	Members   []RoomMember   // Current members (joined and pending), by client ID.
	Envelopes []RoomEnvelope // The caller's, by epoch.
	Invites   []RoomInvite   // The owner's view only.
}

// RoomSummary is one of a chain's rooms.
type RoomSummary struct {
	Room Room
	Me   RoomMember
}

// NewRoom is CreateRoom's input.
type NewRoom struct {
	ID          string
	Owner       string // The chain ID as the token sent it.
	Kind        string
	IdentityPub []byte
	Profile     []byte
	Envelope    []byte // The epoch 1 key sealed to the owner.
}

// NewRoomMember is an email invite (AddRoomMembers).
type NewRoomMember struct {
	ClientID    string
	Role        string
	IdentityPub []byte
	Envelopes   []RoomEnvelope // ClientID ignored.
}

// DirectoryEntry is one account of a signed-in user.
type DirectoryEntry struct {
	ClientID    string
	IdentityPub []byte
	Name        string
	Email       string
}

// RoomChange says who to tell about a room change: the current members
// (joined and pending) and, after a removal, the one removed.
type RoomChange struct {
	Seq     int64
	Members []string
}

func ms(t time.Time) int64 { return t.UnixMilli() }

func scanRoom(row rowScanner, extra ...any) (Room, error) {
	var r Room
	var created int64
	dest := append([]any{&r.ID, &r.Owner, &r.QuotaChain, &r.Kind, &created, &r.Seq, &r.Epoch, &r.Bytes}, extra...)
	if err := row.Scan(dest...); err != nil {
		return r, err
	}
	r.Created = time.UnixMilli(created)
	return r, nil
}

const roomColumns = `id, owner_client_id, quota_chain, kind, created, seq, epoch, bytes`

const memberColumns = `client_id, role, identity_pub, profile, invited, joined, removed`

func scanMember(row rowScanner) (RoomMember, error) {
	var m RoomMember
	var invited, joined, removed int64
	if err := row.Scan(&m.ClientID, &m.Role, &m.IdentityPub, &m.Profile, &invited, &joined, &removed); err != nil {
		return m, err
	}
	m.Invited = time.UnixMilli(invited)
	if joined != 0 {
		m.Joined = time.UnixMilli(joined)
	}
	if removed != 0 {
		m.Removed = time.UnixMilli(removed)
	}
	return m, nil
}

type querier interface {
	QueryRowContext(ctx context.Context, query string, args ...any) *sql.Row
	QueryContext(ctx context.Context, query string, args ...any) (*sql.Rows, error)
}

func loadRoom(ctx context.Context, q querier, id string) (Room, error) {
	r, err := scanRoom(q.QueryRowContext(ctx, `SELECT `+roomColumns+` FROM rooms WHERE id = ?`, id))
	if errors.Is(err, sql.ErrNoRows) {
		return r, ErrRoomNotFound
	}
	if err != nil {
		return r, fmt.Errorf("error reading room: %w", err)
	}
	return r, nil
}

func loadMember(ctx context.Context, q querier, roomID, clientID string) (*RoomMember, error) {
	m, err := scanMember(q.QueryRowContext(ctx,
		`SELECT `+memberColumns+` FROM room_members WHERE room_id = ? AND client_id = ?`,
		roomID, strings.ToLower(clientID)))
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("error reading room member: %w", err)
	}
	return &m, nil
}

// access loads the room and the caller's membership: ErrRoomNotFound if
// either is missing, ErrRoomMemberRemoved if the caller was removed,
// ErrRoomNotJoined if pending and !pendingOK, ErrRoomForbidden if its role
// isn't in |roles| (none: any).
func access(ctx context.Context, q querier, roomID, caller string, pendingOK bool, roles ...string) (Room, *RoomMember, error) {
	room, err := loadRoom(ctx, q, roomID)
	if err != nil {
		return room, nil, err
	}
	m, err := loadMember(ctx, q, roomID, caller)
	if err != nil {
		return room, nil, err
	}
	switch {
	case m == nil:
		return room, nil, ErrRoomNotFound
	case !m.Removed.IsZero():
		return room, m, ErrRoomMemberRemoved
	case m.Pending() && !pendingOK:
		return room, m, ErrRoomNotJoined
	case len(roles) > 0 && !containsStr(roles, m.Role):
		return room, m, ErrRoomForbidden
	}
	return room, m, nil
}

func containsStr(list []string, v string) bool {
	for _, s := range list {
		if s == v {
			return true
		}
	}
	return false
}

func activeMembers(ctx context.Context, q querier, roomID string) ([]RoomMember, error) {
	rows, err := q.QueryContext(ctx, `SELECT `+memberColumns+` FROM room_members
		WHERE room_id = ? AND removed = 0 ORDER BY client_id`, roomID)
	if err != nil {
		return nil, fmt.Errorf("error reading room members: %w", err)
	}
	defer rows.Close()
	var out []RoomMember
	for rows.Next() {
		m, err := scanMember(rows)
		if err != nil {
			return nil, fmt.Errorf("error reading room members: %w", err)
		}
		out = append(out, m)
	}
	return out, rows.Err()
}

func memberIDs(ms []RoomMember) []string {
	out := make([]string, 0, len(ms))
	for _, m := range ms {
		out = append(out, m.ClientID)
	}
	return out
}

func (s *SQLite) change(ctx context.Context, tx *sql.Tx, roomID string, extra ...string) (RoomChange, error) {
	members, err := activeMembers(ctx, tx, roomID)
	if err != nil {
		return RoomChange{}, err
	}
	var seq int64
	if err = tx.QueryRowContext(ctx, `SELECT seq FROM rooms WHERE id = ?`, roomID).Scan(&seq); err != nil &&
		!errors.Is(err, sql.ErrNoRows) {
		return RoomChange{}, fmt.Errorf("error reading room: %w", err)
	}
	ids := memberIDs(members)
	for _, e := range extra {
		if !containsStr(ids, e) {
			ids = append(ids, e)
		}
	}
	return RoomChange{Seq: seq, Members: ids}, nil
}

// quotaChain returns the chains row chain |clientID| (any case) is stored
// under, so a room's bytes count against the chain's own quota: the row
// with the most bytes when devices sent it in several cases; |asSent| when
// it has none yet.
func quotaChain(ctx context.Context, tx *sql.Tx, clientID, asSent string) (string, error) {
	var id string
	err := tx.QueryRowContext(ctx, `SELECT client_id FROM chains WHERE lower(client_id) = lower(?)
		ORDER BY bytes DESC, client_id LIMIT 1`, clientID).Scan(&id)
	if errors.Is(err, sql.ErrNoRows) {
		return asSent, nil
	}
	if err != nil {
		return "", fmt.Errorf("error finding chain: %w", err)
	}
	return id, nil
}

func setIdentity(ctx context.Context, tx *sql.Tx, clientID string, pub []byte, now time.Time) error {
	if _, err := tx.ExecContext(ctx, `
		INSERT INTO identity_keys (client_id, pub, updated) VALUES (?, ?, ?)
		ON CONFLICT (client_id) DO UPDATE SET pub = excluded.pub, updated = excluded.updated
		WHERE identity_keys.pub IS NOT excluded.pub`,
		strings.ToLower(clientID), pub, ms(now)); err != nil {
		return fmt.Errorf("error storing identity key: %w", err)
	}
	return nil
}

func existsTx(ctx context.Context, tx *sql.Tx, query string, args ...any) (bool, error) {
	var one int
	err := tx.QueryRowContext(ctx, query, args...).Scan(&one)
	if errors.Is(err, sql.ErrNoRows) {
		return false, nil
	}
	if err != nil {
		return false, fmt.Errorf("error checking if item exists: %w", err)
	}
	return true, nil
}

func (s *SQLite) begin(ctx context.Context) (*sql.Tx, error) {
	tx, err := s.w.BeginTx(ctx, nil)
	if err != nil {
		return nil, fmt.Errorf("error starting transaction: %w", err)
	}
	return tx, nil
}

func commit(tx *sql.Tx) error {
	if err := tx.Commit(); err != nil {
		return fmt.Errorf("error committing: %w", err)
	}
	return nil
}

// CreateRoom creates room |n.ID| (ErrRoomExists if taken) owned by |n.Owner| at epoch 1, with the
// owner's envelope, and records the owner's identity key.
func (s *SQLite) CreateRoom(ctx context.Context, n NewRoom, now time.Time) (Room, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return Room{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	owner := strings.ToLower(n.Owner)
	var owned int
	if err = tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM rooms WHERE owner_client_id = ?`, owner).
		Scan(&owned); err != nil {
		return Room{}, fmt.Errorf("error counting rooms: %w", err)
	}
	if owned >= MaxRoomsPerOwner {
		return Room{}, ErrRoomLimit
	}
	taken, err := existsTx(ctx, tx, `SELECT 1 FROM rooms WHERE id = ?`, n.ID)
	if err != nil {
		return Room{}, err
	}
	if taken {
		return Room{}, ErrRoomExists
	}
	qc, err := quotaChain(ctx, tx, owner, n.Owner)
	if err != nil {
		return Room{}, err
	}
	if _, err = tx.ExecContext(ctx, `INSERT INTO rooms (id, owner_client_id, quota_chain, kind, created, seq, epoch, bytes)
		VALUES (?, ?, ?, ?, ?, 0, 1, 0)`, n.ID, owner, qc, n.Kind, ms(now)); err != nil {
		return Room{}, fmt.Errorf("error creating room: %w", err)
	}
	if _, err = tx.ExecContext(ctx, `INSERT INTO room_members
		(room_id, client_id, role, identity_pub, profile, invited, joined, removed)
		VALUES (?, ?, ?, ?, ?, ?, ?, 0)`,
		n.ID, owner, RoleOwner, n.IdentityPub, blob(n.Profile), ms(now), ms(now)); err != nil {
		return Room{}, fmt.Errorf("error adding the owner: %w", err)
	}
	if _, err = tx.ExecContext(ctx, `INSERT INTO room_envelopes (room_id, client_id, epoch, sealed)
		VALUES (?, ?, 1, ?)`, n.ID, owner, n.Envelope); err != nil {
		return Room{}, fmt.Errorf("error storing envelope: %w", err)
	}
	if err = setIdentity(ctx, tx, owner, n.IdentityPub, now); err != nil {
		return Room{}, err
	}
	room, err := loadRoom(ctx, tx, n.ID)
	if err != nil {
		return room, err
	}
	return room, commit(tx)
}

// GetRoom returns room |roomID| as member |caller| sees it (pending
// members too).
func (s *SQLite) GetRoom(ctx context.Context, roomID, caller string) (*RoomView, error) {
	tx, err := s.r.BeginTx(ctx, nil)
	if err != nil {
		return nil, fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // read-only
	room, me, err := access(ctx, tx, roomID, caller, true)
	if err != nil {
		return nil, err
	}
	v := &RoomView{Room: room, Me: *me}
	if v.Members, err = activeMembers(ctx, tx, roomID); err != nil {
		return nil, err
	}
	rows, err := tx.QueryContext(ctx, `SELECT epoch, sealed FROM room_envelopes
		WHERE room_id = ? AND client_id = ? ORDER BY epoch`, roomID, me.ClientID)
	if err != nil {
		return nil, fmt.Errorf("error reading envelopes: %w", err)
	}
	for rows.Next() {
		e := RoomEnvelope{ClientID: me.ClientID}
		if err = rows.Scan(&e.Epoch, &e.Sealed); err != nil {
			rows.Close()
			return nil, fmt.Errorf("error reading envelopes: %w", err)
		}
		v.Envelopes = append(v.Envelopes, e)
	}
	rows.Close()
	if me.Role == RoleOwner {
		if v.Invites, err = roomInvites(ctx, tx, roomID); err != nil {
			return nil, err
		}
	}
	return v, nil
}

func roomInvites(ctx context.Context, q querier, roomID string) ([]RoomInvite, error) {
	rows, err := q.QueryContext(ctx, `SELECT id, secret_hash, role, created, expires, uses_left, revoked
		FROM room_invites WHERE room_id = ? ORDER BY created, id`, roomID)
	if err != nil {
		return nil, fmt.Errorf("error reading invites: %w", err)
	}
	defer rows.Close()
	var out []RoomInvite
	for rows.Next() {
		var inv RoomInvite
		var created, expires int64
		var uses sql.NullInt64
		if err = rows.Scan(&inv.ID, &inv.SecretHash, &inv.Role, &created, &expires, &uses, &inv.Revoked); err != nil {
			return nil, fmt.Errorf("error reading invites: %w", err)
		}
		inv.Created = time.UnixMilli(created)
		if expires != 0 {
			inv.Expires = time.UnixMilli(expires)
		}
		inv.UsesLeft = int64Ptr(uses)
		out = append(out, inv)
	}
	return out, rows.Err()
}

// RoomsOf returns the rooms chain |clientID| is a member of (pending
// too), by room ID.
func (s *SQLite) RoomsOf(ctx context.Context, clientID string) ([]RoomSummary, error) {
	rows, err := s.r.QueryContext(ctx, `SELECT r.id, r.owner_client_id, r.quota_chain, r.kind, r.created,
			r.seq, r.epoch, r.bytes, m.client_id, m.role, m.identity_pub, m.profile, m.invited, m.joined, m.removed
		FROM room_members m JOIN rooms r ON r.id = m.room_id
		WHERE m.client_id = ? AND m.removed = 0 ORDER BY r.id`, strings.ToLower(clientID))
	if err != nil {
		return nil, fmt.Errorf("error listing rooms: %w", err)
	}
	defer rows.Close()
	var out []RoomSummary
	for rows.Next() {
		var sum RoomSummary
		var invited, joined, removed int64
		m := &sum.Me
		sum.Room, err = scanRoom(rows, &m.ClientID, &m.Role, &m.IdentityPub, &m.Profile, &invited, &joined, &removed)
		if err != nil {
			return nil, fmt.Errorf("error listing rooms: %w", err)
		}
		m.Invited = time.UnixMilli(invited)
		if joined != 0 {
			m.Joined = time.UnixMilli(joined)
		}
		out = append(out, sum)
	}
	return out, rows.Err()
}

// IsRoomMember says whether chain |clientID| is a current member (joined
// or pending) of room |roomID|.
func (s *SQLite) IsRoomMember(ctx context.Context, roomID, clientID string) (bool, error) {
	return s.exists(ctx, `SELECT 1 FROM room_members WHERE room_id = ? AND client_id = ? AND removed = 0`,
		roomID, strings.ToLower(clientID))
}

// WasRoomMember says whether chain |clientID| is or was (removed) a member
// of room |roomID|, pending included.
func (s *SQLite) WasRoomMember(ctx context.Context, roomID, clientID string) (bool, error) {
	return s.exists(ctx, `SELECT 1 FROM room_members WHERE room_id = ? AND client_id = ?`,
		roomID, strings.ToLower(clientID))
}

// HasRooms says whether chain |clientID| is a current member of any room.
func (s *SQLite) HasRooms(ctx context.Context, clientID string) (bool, error) {
	return s.exists(ctx, `SELECT 1 FROM room_members WHERE client_id = ? AND removed = 0 LIMIT 1`,
		strings.ToLower(clientID))
}

// deleteRoom deletes a room and gives its bytes back to the owner's chain.
func deleteRoom(ctx context.Context, tx *sql.Tx, room Room) error {
	if room.Bytes != 0 {
		if _, err := tx.ExecContext(ctx, `UPDATE chains SET bytes = MAX(bytes - ?, 0) WHERE client_id = ?`,
			room.Bytes, room.QuotaChain); err != nil {
			return fmt.Errorf("error updating chain size: %w", err)
		}
	}
	// The other tables go by ON DELETE CASCADE.
	if _, err := tx.ExecContext(ctx, `DELETE FROM rooms WHERE id = ?`, room.ID); err != nil {
		return fmt.Errorf("error deleting room: %w", err)
	}
	return nil
}

// DeleteRoom deletes room |roomID|: |caller| must be its owner (empty: an
// admin). Returns the members to tell.
func (s *SQLite) DeleteRoom(ctx context.Context, roomID, caller string) (RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	var room Room
	if caller == "" {
		room, err = loadRoom(ctx, tx, roomID)
	} else {
		room, _, err = access(ctx, tx, roomID, caller, false, RoleOwner)
	}
	if err != nil {
		return RoomChange{}, err
	}
	ch, err := s.change(ctx, tx, roomID)
	if err != nil {
		return ch, err
	}
	if err = deleteRoom(ctx, tx, room); err != nil {
		return ch, err
	}
	return ch, commit(tx)
}

// CommitRoomRecords stores |writes| in room |roomID| for |author|, an
// editor or the owner, all or nothing: each gets the next sequence number
// (last write wins). A write whose BaseSeq doesn't match refuses the
// commit with a *RoomConflictError; one of another epoch than the room's
// with ErrStaleEpoch; growth past the owner chain's quota with
// ErrQuotaExceeded. Returns the stored records (without ciphertext).
func (s *SQLite) CommitRoomRecords(ctx context.Context, roomID, author string, writes []RoomRecordWrite) (
	[]RoomRecord, RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return nil, RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	room, me, err := access(ctx, tx, roomID, author, false, RoleOwner, RoleEditor)
	if err != nil {
		return nil, RoomChange{}, err
	}
	var conflicts []RoomRecord
	olds := make([]*RoomRecord, len(writes))
	oldSizes := make([]int64, len(writes))
	for i, w := range writes {
		if w.Epoch != room.Epoch {
			return nil, RoomChange{}, ErrStaleEpoch
		}
		var old RoomRecord
		var size int64
		err = tx.QueryRowContext(ctx, `SELECT record_id, seq, epoch, deleted, author, ciphertext, size
			FROM room_records WHERE room_id = ? AND record_id = ?`, roomID, w.ID).
			Scan(&old.ID, &old.Seq, &old.Epoch, &old.Deleted, &old.Author, &old.Ciphertext, &size)
		switch {
		case errors.Is(err, sql.ErrNoRows):
			if w.BaseSeq != nil && *w.BaseSeq != 0 {
				conflicts = append(conflicts, RoomRecord{ID: w.ID})
			}
		case err != nil:
			return nil, RoomChange{}, fmt.Errorf("error reading record: %w", err)
		default:
			olds[i], oldSizes[i] = &old, size
			if w.BaseSeq != nil && *w.BaseSeq != old.Seq {
				conflicts = append(conflicts, old)
			}
		}
	}
	if len(conflicts) > 0 {
		return nil, RoomChange{}, &RoomConflictError{Current: conflicts}
	}
	var delta int64
	out := make([]RoomRecord, 0, len(writes))
	seq := room.Seq
	for i, w := range writes {
		seq++
		ct := w.Ciphertext
		if w.Deleted {
			ct = nil
		}
		size := int64(len(ct) + len(w.ID))
		delta += size - oldSizes[i]
		if _, err = tx.ExecContext(ctx, `
			INSERT INTO room_records (room_id, record_id, seq, epoch, deleted, author, ciphertext, size)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?)
			ON CONFLICT (room_id, record_id) DO UPDATE SET seq = excluded.seq, epoch = excluded.epoch,
				deleted = excluded.deleted, author = excluded.author, ciphertext = excluded.ciphertext,
				size = excluded.size`,
			roomID, w.ID, seq, w.Epoch, w.Deleted, me.ClientID, blob(ct), size); err != nil {
			return nil, RoomChange{}, fmt.Errorf("error storing record: %w", err)
		}
		out = append(out, RoomRecord{ID: w.ID, Seq: seq, Epoch: w.Epoch, Deleted: w.Deleted, Author: me.ClientID})
	}
	if err = s.addBytes(ctx, tx, room.QuotaChain, delta); err != nil {
		return nil, RoomChange{}, err
	}
	if _, err = tx.ExecContext(ctx, `UPDATE rooms SET seq = ?, bytes = MAX(bytes + ?, 0) WHERE id = ?`,
		seq, delta, roomID); err != nil {
		return nil, RoomChange{}, fmt.Errorf("error updating room: %w", err)
	}
	ch, err := s.change(ctx, tx, roomID)
	if err != nil {
		return nil, ch, err
	}
	return out, ch, commit(tx)
}

// RoomChanges returns the records of room |roomID| changed after |since|,
// oldest first, at most |limit| (and about 8 MB of ciphertext), for a
// joined member; the room's current sequence number and epoch; and
// whether more follow.
func (s *SQLite) RoomChanges(ctx context.Context, roomID, caller string, since int64, limit int) (
	records []RoomRecord, room Room, more bool, err error) {
	tx, err := s.r.BeginTx(ctx, nil)
	if err != nil {
		return nil, room, false, fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // read-only
	if room, _, err = access(ctx, tx, roomID, caller, false); err != nil {
		return nil, room, false, err
	}
	rows, err := tx.QueryContext(ctx, `SELECT record_id, seq, epoch, deleted, author, ciphertext
		FROM room_records WHERE room_id = ? AND seq > ? ORDER BY seq LIMIT ?`, roomID, since, limit+1)
	if err != nil {
		return nil, room, false, fmt.Errorf("error reading records: %w", err)
	}
	defer rows.Close()
	records = []RoomRecord{}
	var total int
	for rows.Next() {
		if len(records) == limit || (len(records) > 0 && total >= maxRoomChangesBytes) {
			more = true
			break
		}
		var r RoomRecord
		if err = rows.Scan(&r.ID, &r.Seq, &r.Epoch, &r.Deleted, &r.Author, &r.Ciphertext); err != nil {
			return nil, room, false, fmt.Errorf("error reading records: %w", err)
		}
		total += len(r.Ciphertext)
		records = append(records, r)
	}
	return records, room, more, rows.Err()
}

// CreateRoomInvite adds link invite |inv| to room |roomID|; |caller| must
// be the owner.
func (s *SQLite) CreateRoomInvite(ctx context.Context, roomID, caller string, inv RoomInvite) (RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	if _, _, err = access(ctx, tx, roomID, caller, false, RoleOwner); err != nil {
		return RoomChange{}, err
	}
	var n int
	if err = tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM room_invites WHERE room_id = ? AND NOT revoked`,
		roomID).Scan(&n); err != nil {
		return RoomChange{}, fmt.Errorf("error counting invites: %w", err)
	}
	if n >= MaxRoomInvites {
		return RoomChange{}, ErrInviteLimit
	}
	var expires int64
	if !inv.Expires.IsZero() {
		expires = ms(inv.Expires)
	}
	if _, err = tx.ExecContext(ctx, `INSERT INTO room_invites
		(room_id, id, secret_hash, role, created, expires, uses_left, revoked) VALUES (?, ?, ?, ?, ?, ?, ?, 0)`,
		roomID, inv.ID, inv.SecretHash, inv.Role, ms(inv.Created), expires, inv.UsesLeft); err != nil {
		return RoomChange{}, fmt.Errorf("error storing invite: %w", err)
	}
	return RoomChange{Members: []string{strings.ToLower(caller)}}, commit(tx)
}

// RevokeRoomInvite revokes invite |inviteID| of room |roomID|; |caller|
// must be the owner. ErrInviteInvalid if there's no such invite.
func (s *SQLite) RevokeRoomInvite(ctx context.Context, roomID, caller, inviteID string) (RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	if _, _, err = access(ctx, tx, roomID, caller, false, RoleOwner); err != nil {
		return RoomChange{}, err
	}
	res, err := tx.ExecContext(ctx, `UPDATE room_invites SET revoked = 1 WHERE room_id = ? AND id = ?`,
		roomID, inviteID)
	if err != nil {
		return RoomChange{}, fmt.Errorf("error revoking invite: %w", err)
	}
	if rowsAffected(res) == 0 {
		return RoomChange{}, ErrInviteInvalid
	}
	return RoomChange{Members: []string{strings.ToLower(caller)}}, commit(tx)
}

func countMembers(ctx context.Context, tx *sql.Tx, roomID string) (int, error) {
	var n int
	if err := tx.QueryRowContext(ctx, `SELECT COUNT(*) FROM room_members WHERE room_id = ? AND removed = 0`,
		roomID).Scan(&n); err != nil {
		return 0, fmt.Errorf("error counting members: %w", err)
	}
	return n, nil
}

// checkInvite verifies a link invite's secret (constant time) and that it
// may still be used at |now|.
func checkInvite(ctx context.Context, tx *sql.Tx, roomID, inviteID string, secret []byte, now time.Time) (
	*RoomInvite, error) {
	var inv RoomInvite
	var created, expires int64
	var uses sql.NullInt64
	err := tx.QueryRowContext(ctx, `SELECT id, secret_hash, role, created, expires, uses_left, revoked
		FROM room_invites WHERE room_id = ? AND id = ?`, roomID, inviteID).
		Scan(&inv.ID, &inv.SecretHash, &inv.Role, &created, &expires, &uses, &inv.Revoked)
	sum := sha256.Sum256(secret)
	if errors.Is(err, sql.ErrNoRows) {
		// Compare anyway, so a wrong ID and a wrong secret take as long.
		subtle.ConstantTimeCompare(sum[:], make([]byte, sha256.Size))
		return nil, ErrInviteInvalid
	}
	if err != nil {
		return nil, fmt.Errorf("error reading invite: %w", err)
	}
	if subtle.ConstantTimeCompare(sum[:], inv.SecretHash) != 1 {
		return nil, ErrInviteInvalid
	}
	inv.Created = time.UnixMilli(created)
	inv.UsesLeft = int64Ptr(uses)
	switch {
	case inv.Revoked:
		return nil, ErrInviteRevoked
	case expires != 0 && !now.Before(time.UnixMilli(expires)):
		return nil, ErrInviteExpired
	case inv.UsesLeft != nil && *inv.UsesLeft <= 0:
		return nil, ErrInviteUsedUp
	}
	return &inv, nil
}

// CheckRoomInvite says whether |secret| opens invite |inviteID| of room
// |roomID| now (the private server's gate for a chain it doesn't list).
func (s *SQLite) CheckRoomInvite(ctx context.Context, roomID, inviteID string, secret []byte, now time.Time) error {
	tx, err := s.r.BeginTx(ctx, nil)
	if err != nil {
		return fmt.Errorf("error starting transaction: %w", err)
	}
	defer tx.Rollback() //nolint:errcheck // read-only
	_, err = checkInvite(ctx, tx, roomID, inviteID, secret, now)
	return err
}

// JoinRoom makes |clientID| a member of room |roomID|:
//   - a current joined member: nothing changes (no invite needed);
//   - a pending member (email invite): joins, no invite needed;
//   - anyone else needs link invite |inviteID| with |secret|, and gets its
//     role (one use). A removed member needs an invite made after the
//     removal (ErrRoomMemberRemoved otherwise).
//
// The member's identity key (and profile, if given) is recorded.
func (s *SQLite) JoinRoom(ctx context.Context, roomID, clientID, inviteID string, secret, identityPub, profile []byte,
	now time.Time) (RoomChange, bool, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, false, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	if _, err = loadRoom(ctx, tx, roomID); err != nil {
		return RoomChange{}, false, err
	}
	id := strings.ToLower(clientID)
	m, err := loadMember(ctx, tx, roomID, id)
	if err != nil {
		return RoomChange{}, false, err
	}
	if m != nil && m.Removed.IsZero() && !m.Pending() {
		return RoomChange{}, false, nil
	}
	if m != nil && m.Removed.IsZero() { // Pending.
		if m.IdentityPub != nil && subtle.ConstantTimeCompare(m.IdentityPub, identityPub) != 1 {
			return RoomChange{}, false, ErrIdentityMismatch
		}
		if _, err = tx.ExecContext(ctx, `UPDATE room_members SET joined = ?, identity_pub = ?,
			profile = COALESCE(?, profile) WHERE room_id = ? AND client_id = ?`,
			ms(now), identityPub, blob(profile), roomID, id); err != nil {
			return RoomChange{}, false, fmt.Errorf("error joining room: %w", err)
		}
	} else {
		if inviteID == "" {
			if m != nil {
				return RoomChange{}, false, ErrRoomMemberRemoved
			}
			return RoomChange{}, false, ErrInviteInvalid
		}
		inv, err := checkInvite(ctx, tx, roomID, inviteID, secret, now)
		if err != nil {
			return RoomChange{}, false, err
		}
		if m != nil && !inv.Created.After(m.Removed) {
			return RoomChange{}, false, ErrRoomMemberRemoved
		}
		n, err := countMembers(ctx, tx, roomID)
		if err != nil {
			return RoomChange{}, false, err
		}
		if n >= MaxRoomMembers {
			return RoomChange{}, false, ErrRoomFull
		}
		if _, err = tx.ExecContext(ctx, `INSERT INTO room_members
			(room_id, client_id, role, identity_pub, profile, invited, joined, removed)
			VALUES (?, ?, ?, ?, ?, ?, ?, 0)
			ON CONFLICT (room_id, client_id) DO UPDATE SET role = excluded.role,
				identity_pub = excluded.identity_pub, profile = excluded.profile,
				invited = excluded.invited, joined = excluded.joined, removed = 0`,
			roomID, id, inv.Role, identityPub, blob(profile), ms(now), ms(now)); err != nil {
			return RoomChange{}, false, fmt.Errorf("error joining room: %w", err)
		}
		if inv.UsesLeft != nil {
			if _, err = tx.ExecContext(ctx, `UPDATE room_invites SET uses_left = uses_left - 1
				WHERE room_id = ? AND id = ?`, roomID, inv.ID); err != nil {
				return RoomChange{}, false, fmt.Errorf("error using invite: %w", err)
			}
		}
	}
	if err = setIdentity(ctx, tx, id, identityPub, now); err != nil {
		return RoomChange{}, false, err
	}
	ch, err := s.change(ctx, tx, roomID)
	if err != nil {
		return ch, false, err
	}
	return ch, true, commit(tx)
}

// AddRoomMembers adds |members| to room |roomID| as pending (email
// invites); |caller| must be the owner. Each must have registered
// |IdentityPub| (ErrNoIdentityKey, ErrIdentityMismatch), not be a current
// member (ErrAlreadyMember), and come with an envelope for the room's
// current epoch (ErrStaleEpoch otherwise); envelopes for earlier epochs are
// kept too.
func (s *SQLite) AddRoomMembers(ctx context.Context, roomID, caller string, members []NewRoomMember,
	now time.Time) (RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	room, _, err := access(ctx, tx, roomID, caller, false, RoleOwner)
	if err != nil {
		return RoomChange{}, err
	}
	n, err := countMembers(ctx, tx, roomID)
	if err != nil {
		return RoomChange{}, err
	}
	if n+len(members) > MaxRoomMembers {
		return RoomChange{}, ErrRoomFull
	}
	for _, nm := range members {
		id := strings.ToLower(nm.ClientID)
		var pub []byte
		err = tx.QueryRowContext(ctx, `SELECT pub FROM identity_keys WHERE client_id = ?`, id).Scan(&pub)
		if errors.Is(err, sql.ErrNoRows) {
			return RoomChange{}, fmt.Errorf("%w: %s", ErrNoIdentityKey, id)
		}
		if err != nil {
			return RoomChange{}, fmt.Errorf("error reading identity key: %w", err)
		}
		if subtle.ConstantTimeCompare(pub, nm.IdentityPub) != 1 {
			return RoomChange{}, fmt.Errorf("%w: %s", ErrIdentityMismatch, id)
		}
		m, err := loadMember(ctx, tx, roomID, id)
		if err != nil {
			return RoomChange{}, err
		}
		if m != nil && m.Removed.IsZero() {
			return RoomChange{}, fmt.Errorf("%w: %s", ErrAlreadyMember, id)
		}
		current := false
		for _, e := range nm.Envelopes {
			if e.Epoch < 1 || e.Epoch > room.Epoch {
				return RoomChange{}, ErrStaleEpoch
			}
			current = current || e.Epoch == room.Epoch
		}
		if !current {
			return RoomChange{}, ErrStaleEpoch
		}
		if _, err = tx.ExecContext(ctx, `INSERT INTO room_members
			(room_id, client_id, role, identity_pub, profile, invited, joined, removed)
			VALUES (?, ?, ?, ?, NULL, ?, 0, 0)
			ON CONFLICT (room_id, client_id) DO UPDATE SET role = excluded.role,
				identity_pub = excluded.identity_pub, profile = NULL, invited = excluded.invited,
				joined = 0, removed = 0`, roomID, id, nm.Role, pub, ms(now)); err != nil {
			return RoomChange{}, fmt.Errorf("error adding member: %w", err)
		}
		for _, e := range nm.Envelopes {
			if err = putEnvelope(ctx, tx, roomID, id, e.Epoch, e.Sealed); err != nil {
				return RoomChange{}, err
			}
		}
	}
	ch, err := s.change(ctx, tx, roomID)
	if err != nil {
		return ch, err
	}
	return ch, commit(tx)
}

func putEnvelope(ctx context.Context, tx *sql.Tx, roomID, clientID string, epoch int64, sealed []byte) error {
	if _, err := tx.ExecContext(ctx, `INSERT INTO room_envelopes (room_id, client_id, epoch, sealed)
		VALUES (?, ?, ?, ?) ON CONFLICT (room_id, client_id, epoch) DO UPDATE SET sealed = excluded.sealed`,
		roomID, strings.ToLower(clientID), epoch, sealed); err != nil {
		return fmt.Errorf("error storing envelope: %w", err)
	}
	return nil
}

func currentTarget(ctx context.Context, tx *sql.Tx, roomID, target string) (*RoomMember, error) {
	m, err := loadMember(ctx, tx, roomID, target)
	if err != nil {
		return nil, err
	}
	if m == nil || !m.Removed.IsZero() {
		return nil, ErrNotRoomMember
	}
	return m, nil
}

// SetRoomMemberRole changes |target|'s role to editor or viewer; |caller|
// must be the owner, and |target| a current member other than the owner
// (ErrRoomForbidden; ownership moves with TransferRoomOwner).
func (s *SQLite) SetRoomMemberRole(ctx context.Context, roomID, caller, target, role string) (RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	if _, _, err = access(ctx, tx, roomID, caller, false, RoleOwner); err != nil {
		return RoomChange{}, err
	}
	m, err := currentTarget(ctx, tx, roomID, target)
	if err != nil {
		return RoomChange{}, err
	}
	if m.Role == RoleOwner {
		return RoomChange{}, ErrRoomForbidden
	}
	if _, err = tx.ExecContext(ctx, `UPDATE room_members SET role = ? WHERE room_id = ? AND client_id = ?`,
		role, roomID, m.ClientID); err != nil {
		return RoomChange{}, fmt.Errorf("error changing role: %w", err)
	}
	ch, err := s.change(ctx, tx, roomID)
	if err != nil {
		return ch, err
	}
	return ch, commit(tx)
}

// SetRoomProfile replaces |caller|'s own profile (an encrypted blob) in
// room |roomID|; pending members may too.
func (s *SQLite) SetRoomProfile(ctx context.Context, roomID, caller string, profile []byte) (RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	_, me, err := access(ctx, tx, roomID, caller, true)
	if err != nil {
		return RoomChange{}, err
	}
	if _, err = tx.ExecContext(ctx, `UPDATE room_members SET profile = ? WHERE room_id = ? AND client_id = ?`,
		blob(profile), roomID, me.ClientID); err != nil {
		return RoomChange{}, fmt.Errorf("error storing profile: %w", err)
	}
	ch, err := s.change(ctx, tx, roomID)
	if err != nil {
		return ch, err
	}
	return ch, commit(tx)
}

// RemoveRoomMember removes |target| from room |roomID|: |caller| is the
// target itself (leaving; a pending member declining) or the owner. The
// owner can't leave (ErrOwnerMustTransfer). The target's envelopes go;
// its row stays, marked removed, so it's told so (ErrRoomMemberRemoved)
// rather than "not found". The change lists the target too.
func (s *SQLite) RemoveRoomMember(ctx context.Context, roomID, caller, target string, now time.Time) (RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	self := strings.EqualFold(caller, target)
	_, me, err := access(ctx, tx, roomID, caller, self)
	if err != nil {
		return RoomChange{}, err
	}
	if !self && me.Role != RoleOwner {
		return RoomChange{}, ErrRoomForbidden
	}
	m, err := currentTarget(ctx, tx, roomID, target)
	if err != nil {
		return RoomChange{}, err
	}
	if m.Role == RoleOwner {
		return RoomChange{}, ErrOwnerMustTransfer
	}
	if _, err = tx.ExecContext(ctx, `UPDATE room_members SET removed = ? WHERE room_id = ? AND client_id = ?`,
		ms(now), roomID, m.ClientID); err != nil {
		return RoomChange{}, fmt.Errorf("error removing member: %w", err)
	}
	if _, err = tx.ExecContext(ctx, `DELETE FROM room_envelopes WHERE room_id = ? AND client_id = ?`,
		roomID, m.ClientID); err != nil {
		return RoomChange{}, fmt.Errorf("error removing envelopes: %w", err)
	}
	ch, err := s.change(ctx, tx, roomID, m.ClientID)
	if err != nil {
		return ch, err
	}
	return ch, commit(tx)
}

// TransferRoomOwner makes |target|, a joined member, the owner of room
// |roomID|; |caller| must be the owner and becomes an editor. The room's
// bytes move to the new owner's chain (ErrQuotaExceeded if they don't
// fit).
func (s *SQLite) TransferRoomOwner(ctx context.Context, roomID, caller, target string) (RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	room, me, err := access(ctx, tx, roomID, caller, false, RoleOwner)
	if err != nil {
		return RoomChange{}, err
	}
	m, err := currentTarget(ctx, tx, roomID, target)
	if err != nil {
		return RoomChange{}, err
	}
	if m.Pending() {
		return RoomChange{}, ErrRoomNotJoined
	}
	if m.ClientID == me.ClientID {
		return RoomChange{}, nil
	}
	qc, err := quotaChain(ctx, tx, m.ClientID, m.ClientID)
	if err != nil {
		return RoomChange{}, err
	}
	if err = s.addBytes(ctx, tx, qc, room.Bytes); err != nil {
		return RoomChange{}, err
	}
	if room.Bytes != 0 {
		if _, err = tx.ExecContext(ctx, `UPDATE chains SET bytes = MAX(bytes - ?, 0) WHERE client_id = ?`,
			room.Bytes, room.QuotaChain); err != nil {
			return RoomChange{}, fmt.Errorf("error updating chain size: %w", err)
		}
	}
	for _, stmt := range []struct {
		q    string
		args []any
	}{
		{`UPDATE rooms SET owner_client_id = ?, quota_chain = ? WHERE id = ?`, []any{m.ClientID, qc, roomID}},
		{`UPDATE room_members SET role = ? WHERE room_id = ? AND client_id = ?`, []any{RoleEditor, roomID, me.ClientID}},
		{`UPDATE room_members SET role = ? WHERE room_id = ? AND client_id = ?`, []any{RoleOwner, roomID, m.ClientID}},
	} {
		if _, err = tx.ExecContext(ctx, stmt.q, stmt.args...); err != nil {
			return RoomChange{}, fmt.Errorf("error transferring room: %w", err)
		}
	}
	ch, err := s.change(ctx, tx, roomID)
	if err != nil {
		return ch, err
	}
	return ch, commit(tx)
}

// RotateRoomEpoch starts epoch |epoch| (the current one plus one, else
// ErrStaleEpoch) of room |roomID| with |envelopes| (client ID → sealed
// key); |caller| must be the owner, and there must be exactly one envelope
// for every current member, joined or pending (*RoomEnvelopeError).
func (s *SQLite) RotateRoomEpoch(ctx context.Context, roomID, caller string, epoch int64,
	envelopes map[string][]byte) (RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	room, _, err := access(ctx, tx, roomID, caller, false, RoleOwner)
	if err != nil {
		return RoomChange{}, err
	}
	if epoch != room.Epoch+1 {
		return RoomChange{}, ErrStaleEpoch
	}
	members, err := activeMembers(ctx, tx, roomID)
	if err != nil {
		return RoomChange{}, err
	}
	byID := map[string][]byte{}
	for id, sealed := range envelopes {
		byID[strings.ToLower(id)] = sealed
	}
	var mismatch RoomEnvelopeError
	for _, m := range members {
		if _, ok := byID[m.ClientID]; !ok {
			mismatch.Missing = append(mismatch.Missing, m.ClientID)
		}
	}
	for id := range byID {
		if !containsStr(memberIDs(members), id) {
			mismatch.Unknown = append(mismatch.Unknown, id)
		}
	}
	if len(mismatch.Missing) > 0 || len(mismatch.Unknown) > 0 {
		sort.Strings(mismatch.Unknown)
		return RoomChange{}, &mismatch
	}
	for _, m := range members {
		if err = putEnvelope(ctx, tx, roomID, m.ClientID, epoch, byID[m.ClientID]); err != nil {
			return RoomChange{}, err
		}
	}
	if _, err = tx.ExecContext(ctx, `UPDATE rooms SET epoch = ? WHERE id = ?`, epoch, roomID); err != nil {
		return RoomChange{}, fmt.Errorf("error rotating room: %w", err)
	}
	ch, err := s.change(ctx, tx, roomID)
	if err != nil {
		return ch, err
	}
	return ch, commit(tx)
}

// AddRoomEnvelopes stores envelopes of existing epochs for current
// members (a member who joined by a link made before a rotation needs the
// newer keys); |caller| must be the owner. ErrNotRoomMember for a target
// that isn't a current member, ErrStaleEpoch for an epoch the room hasn't
// reached.
func (s *SQLite) AddRoomEnvelopes(ctx context.Context, roomID, caller string, envelopes []RoomEnvelope) (
	RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	room, _, err := access(ctx, tx, roomID, caller, false, RoleOwner)
	if err != nil {
		return RoomChange{}, err
	}
	targets := map[string]bool{}
	for _, e := range envelopes {
		if e.Epoch < 1 || e.Epoch > room.Epoch {
			return RoomChange{}, ErrStaleEpoch
		}
		if _, err = currentTarget(ctx, tx, roomID, e.ClientID); err != nil {
			return RoomChange{}, err
		}
		if err = putEnvelope(ctx, tx, roomID, e.ClientID, e.Epoch, e.Sealed); err != nil {
			return RoomChange{}, err
		}
		targets[strings.ToLower(e.ClientID)] = true
	}
	ch := RoomChange{Seq: room.Seq}
	for id := range targets {
		ch.Members = append(ch.Members, id)
	}
	sort.Strings(ch.Members)
	return ch, commit(tx)
}

// SetIdentityKey records chain |clientID|'s identity public key, and
// updates it in the rooms the chain is a current member of. Returns those
// rooms.
func (s *SQLite) SetIdentityKey(ctx context.Context, clientID string, pub []byte, now time.Time) ([]string, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	id := strings.ToLower(clientID)
	if err = setIdentity(ctx, tx, id, pub, now); err != nil {
		return nil, err
	}
	rows, err := tx.QueryContext(ctx, `SELECT room_id FROM room_members
		WHERE client_id = ? AND removed = 0 AND identity_pub IS NOT ? ORDER BY room_id`, id, pub)
	if err != nil {
		return nil, fmt.Errorf("error reading rooms: %w", err)
	}
	rooms, err := collectStrings(rows)
	if err != nil {
		return nil, err
	}
	if _, err = tx.ExecContext(ctx, `UPDATE room_members SET identity_pub = ?
		WHERE client_id = ? AND removed = 0`, pub, id); err != nil {
		return nil, fmt.Errorf("error updating rooms: %w", err)
	}
	return rooms, commit(tx)
}

// IdentityKey returns chain |clientID|'s identity public key, or nil.
func (s *SQLite) IdentityKey(ctx context.Context, clientID string) ([]byte, error) {
	var pub []byte
	err := s.r.QueryRowContext(ctx, `SELECT pub FROM identity_keys WHERE client_id = ?`,
		strings.ToLower(clientID)).Scan(&pub)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("error reading identity key: %w", err)
	}
	return pub, nil
}

// RoomMembersOf returns the current members of room |roomID| (for the
// rooms handler's notifications), or nil if there's no such room.
func (s *SQLite) RoomMembersOf(ctx context.Context, roomID string) ([]string, error) {
	members, err := activeMembers(ctx, s.r, roomID)
	if err != nil {
		return nil, err
	}
	return memberIDs(members), nil
}

// Directory returns the accounts of the users of |issuer| whose email is
// |email| (any case): chains bound to a user not revoked, whose session
// hasn't expired at |now|, and that registered an identity key. By client
// ID.
func (s *SQLite) Directory(ctx context.Context, issuer, email string, now time.Time) ([]DirectoryEntry, error) {
	rows, err := s.r.QueryContext(ctx, `
		SELECT c.client_id, k.pub, u.name, u.email
		FROM oidc_users u
		JOIN chain_logins c ON c.user_id = u.id
		JOIN identity_keys k ON k.client_id = c.client_id
		WHERE u.issuer = ? AND lower(u.email) = lower(?) AND u.email <> '' AND NOT u.revoked
			AND (c.session_expires = 0 OR c.session_expires > ?)
		ORDER BY c.client_id`, issuer, email, ms(now))
	if err != nil {
		return nil, fmt.Errorf("error reading directory: %w", err)
	}
	defer rows.Close()
	out := []DirectoryEntry{}
	for rows.Next() {
		var e DirectoryEntry
		if err = rows.Scan(&e.ClientID, &e.IdentityPub, &e.Name, &e.Email); err != nil {
			return nil, fmt.Errorf("error reading directory: %w", err)
		}
		out = append(out, e)
	}
	return out, rows.Err()
}

// ListRooms returns every room with its member count (admin).
func (s *SQLite) ListRooms(ctx context.Context) ([]Room, error) {
	rows, err := s.r.QueryContext(ctx, `SELECT `+roomColumns+`,
		(SELECT COUNT(*) FROM room_members m WHERE m.room_id = rooms.id AND m.removed = 0)
		FROM rooms ORDER BY created, id`)
	if err != nil {
		return nil, fmt.Errorf("error listing rooms: %w", err)
	}
	defer rows.Close()
	var out []Room
	for rows.Next() {
		var n int
		r, err := scanRoom(rows, &n)
		if err != nil {
			return nil, fmt.Errorf("error listing rooms: %w", err)
		}
		r.Members = n
		out = append(out, r)
	}
	return out, rows.Err()
}

// RoomDetails is what the admin sees of a room.
type RoomDetails struct {
	Room    Room
	Members []RoomMember // Removed ones too.
	Invites []RoomInvite
	Records int // Live records (tombstones not counted).
}

// AdminRoom returns room |roomID| for the admin (ErrRoomNotFound).
func (s *SQLite) AdminRoom(ctx context.Context, roomID string) (*RoomDetails, error) {
	room, err := loadRoom(ctx, s.r, roomID)
	if err != nil {
		return nil, err
	}
	d := &RoomDetails{Room: room}
	rows, err := s.r.QueryContext(ctx, `SELECT `+memberColumns+` FROM room_members
		WHERE room_id = ? ORDER BY removed <> 0, client_id`, roomID)
	if err != nil {
		return nil, fmt.Errorf("error reading room members: %w", err)
	}
	for rows.Next() {
		m, err := scanMember(rows)
		if err != nil {
			rows.Close()
			return nil, fmt.Errorf("error reading room members: %w", err)
		}
		d.Members = append(d.Members, m)
	}
	rows.Close()
	if d.Invites, err = roomInvites(ctx, s.r, roomID); err != nil {
		return nil, err
	}
	if err = s.r.QueryRowContext(ctx, `SELECT COUNT(*) FROM room_records WHERE room_id = ? AND NOT deleted`,
		roomID).Scan(&d.Records); err != nil {
		return nil, fmt.Errorf("error counting records: %w", err)
	}
	return d, nil
}

// clearChainRooms is ClearServerData's part for rooms: the rooms chain
// |clientID| (any case) owns are deleted, its other memberships and its
// envelopes and identity key go. Returns the rooms deleted and the rooms
// it left.
func clearChainRooms(ctx context.Context, tx *sql.Tx, clientID string) (deleted, left []string, err error) {
	id := strings.ToLower(clientID)
	rows, err := tx.QueryContext(ctx, `SELECT `+roomColumns+` FROM rooms WHERE owner_client_id = ?`, id)
	if err != nil {
		return nil, nil, fmt.Errorf("error reading the chain's rooms: %w", err)
	}
	var owned []Room
	for rows.Next() {
		r, err := scanRoom(rows)
		if err != nil {
			rows.Close()
			return nil, nil, fmt.Errorf("error reading the chain's rooms: %w", err)
		}
		owned = append(owned, r)
	}
	rows.Close()
	for _, r := range owned {
		if err = deleteRoom(ctx, tx, r); err != nil {
			return nil, nil, err
		}
		deleted = append(deleted, r.ID)
	}
	rows, err = tx.QueryContext(ctx, `SELECT room_id FROM room_members WHERE client_id = ? AND removed = 0
		ORDER BY room_id`, id)
	if err != nil {
		return nil, nil, fmt.Errorf("error reading the chain's rooms: %w", err)
	}
	if left, err = collectStrings(rows); err != nil {
		return nil, nil, err
	}
	for _, stmt := range []string{
		`DELETE FROM room_members WHERE client_id = ?`,
		`DELETE FROM room_envelopes WHERE client_id = ?`,
		`DELETE FROM identity_keys WHERE client_id = ?`,
	} {
		if _, err = tx.ExecContext(ctx, stmt, id); err != nil {
			return nil, nil, fmt.Errorf("error clearing the chain's rooms: %w", err)
		}
	}
	return deleted, left, nil
}

// MoveRoomMembership hands |caller|'s membership of room |roomID| to
// chain |successor| (as its token sent it): the successor flow of a reset,
// where the account's old chain gives way to a new one. The successor gets
// the caller's role (the owner stays the owner, and the room's bytes move
// to the successor's quota: ErrQuotaExceeded), its profile and pending
// state, identity key |identityPub| and |envelopes|, which must include the
// current epoch (ErrStaleEpoch; epochs from 1 to current). The caller is
// marked removed. ErrAlreadyMember if the successor is a current member.
// Pending members may move too.
func (s *SQLite) MoveRoomMembership(ctx context.Context, roomID, caller, successor string, identityPub []byte,
	envelopes []RoomEnvelope, now time.Time) (RoomChange, error) {
	tx, err := s.begin(ctx)
	if err != nil {
		return RoomChange{}, err
	}
	defer tx.Rollback() //nolint:errcheck // no-op after Commit
	room, me, err := access(ctx, tx, roomID, caller, true)
	if err != nil {
		return RoomChange{}, err
	}
	next := strings.ToLower(successor)
	if m, err := loadMember(ctx, tx, roomID, next); err != nil {
		return RoomChange{}, err
	} else if m != nil && m.Removed.IsZero() {
		return RoomChange{}, ErrAlreadyMember
	}
	current := false
	for _, e := range envelopes {
		if e.Epoch < 1 || e.Epoch > room.Epoch {
			return RoomChange{}, ErrStaleEpoch
		}
		current = current || e.Epoch == room.Epoch
	}
	if !current {
		return RoomChange{}, ErrStaleEpoch
	}
	if me.Role == RoleOwner {
		qc, err := quotaChain(ctx, tx, next, successor)
		if err != nil {
			return RoomChange{}, err
		}
		if err = s.addBytes(ctx, tx, qc, room.Bytes); err != nil {
			return RoomChange{}, err
		}
		if room.Bytes != 0 {
			if _, err = tx.ExecContext(ctx, `UPDATE chains SET bytes = MAX(bytes - ?, 0) WHERE client_id = ?`,
				room.Bytes, room.QuotaChain); err != nil {
				return RoomChange{}, fmt.Errorf("error updating chain size: %w", err)
			}
		}
		if _, err = tx.ExecContext(ctx, `UPDATE rooms SET owner_client_id = ?, quota_chain = ? WHERE id = ?`,
			next, qc, roomID); err != nil {
			return RoomChange{}, fmt.Errorf("error moving the room: %w", err)
		}
	}
	var joined int64
	if !me.Pending() {
		joined = ms(now)
	}
	if _, err = tx.ExecContext(ctx, `INSERT INTO room_members
		(room_id, client_id, role, identity_pub, profile, invited, joined, removed)
		VALUES (?, ?, ?, ?, ?, ?, ?, 0)
		ON CONFLICT (room_id, client_id) DO UPDATE SET role = excluded.role,
			identity_pub = excluded.identity_pub, profile = excluded.profile, invited = excluded.invited,
			joined = excluded.joined, removed = 0`,
		roomID, next, me.Role, identityPub, blob(me.Profile), ms(me.Invited), joined); err != nil {
		return RoomChange{}, fmt.Errorf("error adding the successor: %w", err)
	}
	if _, err = tx.ExecContext(ctx, `DELETE FROM room_envelopes WHERE room_id = ? AND client_id IN (?, ?)`,
		roomID, me.ClientID, next); err != nil {
		return RoomChange{}, fmt.Errorf("error moving envelopes: %w", err)
	}
	for _, e := range envelopes {
		if err = putEnvelope(ctx, tx, roomID, next, e.Epoch, e.Sealed); err != nil {
			return RoomChange{}, err
		}
	}
	if _, err = tx.ExecContext(ctx, `UPDATE room_members SET removed = ?,
		role = CASE WHEN role = ? THEN ? ELSE role END
		WHERE room_id = ? AND client_id = ?`, ms(now), RoleOwner, RoleEditor, roomID, me.ClientID); err != nil {
		return RoomChange{}, fmt.Errorf("error removing the old chain: %w", err)
	}
	if err = setIdentity(ctx, tx, next, identityPub, now); err != nil {
		return RoomChange{}, err
	}
	ch, err := s.change(ctx, tx, roomID, me.ClientID)
	if err != nil {
		return ch, err
	}
	return ch, commit(tx)
}
