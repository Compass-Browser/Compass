package datastore_test

import (
	"context"
	"crypto/sha256"
	"database/sql"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/datastore"
)

var (
	ownerID  = strings.Repeat("a1", 32)
	editorID = strings.Repeat("b2", 32)
	viewerID = strings.Repeat("c3", 32)
	otherID  = strings.Repeat("d4", 32)
	roomNow  = time.UnixMilli(1_800_000_000_000)
)

const roomID = "0123456789abcdef0123456789abcdef"

func pub(b byte) []byte { return []byte(strings.Repeat(string(rune(b)), 32))[:32] }

func i64(v int64) *int64 { return &v }

func secretHash(s string) []byte {
	h := sha256.Sum256([]byte(s))
	return h[:]
}

// newRoom makes a room of |owner| (as Chromium sends it: upper case).
func newRoom(t *testing.T, db *datastore.SQLite, id, owner string) {
	t.Helper()
	_, err := db.CreateRoom(context.Background(), datastore.NewRoom{ID: id, Owner: strings.ToUpper(owner),
		Kind: "folder", IdentityPub: pub('o'), Envelope: []byte("env-owner-1")}, roomNow)
	require.NoError(t, err)
}

// invite adds an invite with secret |secret| and returns its ID.
func invite(t *testing.T, db *datastore.SQLite, room, id, role, secret string, expires time.Time, uses *int64) {
	t.Helper()
	_, err := db.CreateRoomInvite(context.Background(), room, ownerID, datastore.RoomInvite{ID: id,
		SecretHash: secretHash(secret), Role: role, Created: roomNow, Expires: expires, UsesLeft: uses})
	require.NoError(t, err)
}

func join(db *datastore.SQLite, room, who, inviteID, secret string, at time.Time) error {
	_, _, err := db.JoinRoom(context.Background(), room, who, inviteID, []byte(secret), pub(who[0]), nil, at)
	return err
}

// roomWithMembers: owner, an editor and a viewer, all joined.
func roomWithMembers(t *testing.T, db *datastore.SQLite) {
	t.Helper()
	newRoom(t, db, roomID, ownerID)
	invite(t, db, roomID, "inv-e", datastore.RoleEditor, "secret-e", time.Time{}, nil)
	invite(t, db, roomID, "inv-v", datastore.RoleViewer, "secret-v", time.Time{}, nil)
	require.NoError(t, join(db, roomID, editorID, "inv-e", "secret-e", roomNow))
	require.NoError(t, join(db, roomID, viewerID, "inv-v", "secret-v", roomNow))
}

func TestRoomCreateAndView(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	ctx := context.Background()
	roomWithMembers(t, db)

	v, err := db.GetRoom(ctx, roomID, strings.ToUpper(ownerID))
	require.NoError(t, err)
	assert.Equal(t, ownerID, v.Room.Owner, "lower-case")
	assert.Equal(t, strings.ToUpper(ownerID), v.Room.QuotaChain, "as sent: no chains row yet")
	assert.Equal(t, int64(1), v.Room.Epoch)
	assert.Equal(t, datastore.RoleOwner, v.Me.Role)
	require.Len(t, v.Members, 3)
	assert.Equal(t, []datastore.RoomEnvelope{{ClientID: ownerID, Epoch: 1, Sealed: []byte("env-owner-1")}}, v.Envelopes)
	assert.Len(t, v.Invites, 2, "the owner sees the invites")

	v, err = db.GetRoom(ctx, roomID, viewerID)
	require.NoError(t, err)
	assert.Equal(t, datastore.RoleViewer, v.Me.Role)
	assert.Empty(t, v.Invites, "others don't")
	assert.Empty(t, v.Envelopes, "joined by link: the key came in the link")

	_, err = db.GetRoom(ctx, roomID, otherID)
	require.ErrorIs(t, err, datastore.ErrRoomNotFound)
	_, err = db.GetRoom(ctx, "ffffffffffffffffffffffffffffffff", ownerID)
	require.ErrorIs(t, err, datastore.ErrRoomNotFound)

	rooms, err := db.RoomsOf(ctx, editorID)
	require.NoError(t, err)
	require.Len(t, rooms, 1)
	assert.Equal(t, datastore.RoleEditor, rooms[0].Me.Role)
	key, err := db.IdentityKey(ctx, strings.ToUpper(editorID))
	require.NoError(t, err)
	assert.Equal(t, pub(editorID[0]), key)
}

func TestRoomRolePermissions(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	ctx := context.Background()
	roomWithMembers(t, db)
	write := []datastore.RoomRecordWrite{{ID: "n1", Epoch: 1, Ciphertext: []byte("x")}}

	for _, who := range []string{ownerID, editorID} {
		_, _, err := db.CommitRoomRecords(ctx, roomID, who, write)
		require.NoError(t, err, who)
	}
	_, _, err := db.CommitRoomRecords(ctx, roomID, viewerID, write)
	require.ErrorIs(t, err, datastore.ErrRoomForbidden)
	_, _, err = db.CommitRoomRecords(ctx, roomID, otherID, write)
	require.ErrorIs(t, err, datastore.ErrRoomNotFound)

	// Everyone joined reads.
	for _, who := range []string{ownerID, editorID, viewerID} {
		recs, _, _, err := db.RoomChanges(ctx, roomID, who, 0, 100)
		require.NoError(t, err)
		assert.Len(t, recs, 1)
	}

	// Owner-only operations refuse the others.
	for _, who := range []string{editorID, viewerID} {
		_, err = db.CreateRoomInvite(ctx, roomID, who, datastore.RoomInvite{ID: "x", SecretHash: secretHash("s"),
			Role: datastore.RoleEditor, Created: roomNow})
		require.ErrorIs(t, err, datastore.ErrRoomForbidden)
		_, err = db.RevokeRoomInvite(ctx, roomID, who, "inv-e")
		require.ErrorIs(t, err, datastore.ErrRoomForbidden)
		_, err = db.SetRoomMemberRole(ctx, roomID, who, viewerID, datastore.RoleEditor)
		require.ErrorIs(t, err, datastore.ErrRoomForbidden)
		_, err = db.RemoveRoomMember(ctx, roomID, who, ownerID, roomNow)
		require.ErrorIs(t, err, datastore.ErrRoomForbidden)
		_, err = db.TransferRoomOwner(ctx, roomID, who, who)
		require.ErrorIs(t, err, datastore.ErrRoomForbidden)
		_, err = db.RotateRoomEpoch(ctx, roomID, who, 2, nil)
		require.ErrorIs(t, err, datastore.ErrRoomForbidden)
		_, err = db.AddRoomEnvelopes(ctx, roomID, who, []datastore.RoomEnvelope{{ClientID: who, Epoch: 1, Sealed: []byte("e")}})
		require.ErrorIs(t, err, datastore.ErrRoomForbidden)
		_, err = db.AddRoomMembers(ctx, roomID, who, nil, roomNow)
		require.ErrorIs(t, err, datastore.ErrRoomForbidden)
		_, err = db.DeleteRoom(ctx, roomID, who)
		require.ErrorIs(t, err, datastore.ErrRoomForbidden)
	}
	// The editor can't remove the viewer either; each sets only its own profile.
	_, err = db.RemoveRoomMember(ctx, roomID, editorID, viewerID, roomNow)
	require.ErrorIs(t, err, datastore.ErrRoomForbidden)
	_, err = db.SetRoomProfile(ctx, roomID, viewerID, []byte("me"))
	require.NoError(t, err)

	// The owner changes roles; never its own.
	_, err = db.SetRoomMemberRole(ctx, roomID, ownerID, viewerID, datastore.RoleEditor)
	require.NoError(t, err)
	_, _, err = db.CommitRoomRecords(ctx, roomID, viewerID, write)
	require.NoError(t, err, "promoted")
	_, err = db.SetRoomMemberRole(ctx, roomID, ownerID, ownerID, datastore.RoleViewer)
	require.ErrorIs(t, err, datastore.ErrRoomForbidden)
	_, err = db.SetRoomMemberRole(ctx, roomID, ownerID, otherID, datastore.RoleViewer)
	require.ErrorIs(t, err, datastore.ErrNotRoomMember)
}

func TestRoomInvites(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	ctx := context.Background()
	newRoom(t, db, roomID, ownerID)
	invite(t, db, roomID, "once", datastore.RoleViewer, "s-once", time.Time{}, i64(1))
	invite(t, db, roomID, "soon", datastore.RoleEditor, "s-soon", roomNow.Add(time.Hour), nil)
	invite(t, db, roomID, "gone", datastore.RoleEditor, "s-gone", time.Time{}, nil)
	_, err := db.RevokeRoomInvite(ctx, roomID, ownerID, "gone")
	require.NoError(t, err)
	_, err = db.RevokeRoomInvite(ctx, roomID, ownerID, "nope")
	require.ErrorIs(t, err, datastore.ErrInviteInvalid)

	require.ErrorIs(t, join(db, roomID, editorID, "once", "wrong", roomNow), datastore.ErrInviteInvalid)
	require.ErrorIs(t, join(db, roomID, editorID, "nope", "s-once", roomNow), datastore.ErrInviteInvalid)
	require.ErrorIs(t, join(db, roomID, editorID, "", "", roomNow), datastore.ErrInviteInvalid)
	require.ErrorIs(t, join(db, roomID, editorID, "gone", "s-gone", roomNow), datastore.ErrInviteRevoked)
	require.ErrorIs(t, join(db, roomID, editorID, "soon", "s-soon", roomNow.Add(time.Hour)), datastore.ErrInviteExpired)
	require.ErrorIs(t, db.CheckRoomInvite(ctx, roomID, "once", []byte("wrong"), roomNow), datastore.ErrInviteInvalid)
	require.NoError(t, db.CheckRoomInvite(ctx, roomID, "once", []byte("s-once"), roomNow))

	require.NoError(t, join(db, roomID, viewerID, "once", "s-once", roomNow))
	require.ErrorIs(t, join(db, roomID, otherID, "once", "s-once", roomNow), datastore.ErrInviteUsedUp)
	// A member joining again: nothing changes, no use taken.
	ch, joined, err := db.JoinRoom(ctx, roomID, viewerID, "", nil, pub('v'), nil, roomNow)
	require.NoError(t, err)
	assert.False(t, joined)
	assert.Empty(t, ch.Members)

	require.NoError(t, join(db, roomID, editorID, "soon", "s-soon", roomNow.Add(time.Minute)))
	v, err := db.GetRoom(ctx, roomID, editorID)
	require.NoError(t, err)
	assert.Equal(t, datastore.RoleEditor, v.Me.Role, "the invite's role")
	v, err = db.GetRoom(ctx, roomID, ownerID)
	require.NoError(t, err)
	for _, inv := range v.Invites {
		switch inv.ID {
		case "once":
			assert.Equal(t, int64(0), *inv.UsesLeft)
		case "gone":
			assert.True(t, inv.Revoked)
		case "soon":
			assert.Nil(t, inv.UsesLeft)
			assert.Equal(t, roomNow.Add(time.Hour), inv.Expires)
		}
	}
	// No such room.
	_, _, err = db.JoinRoom(ctx, "ffffffffffffffffffffffffffffffff", otherID, "once", []byte("s-once"), pub('x'), nil, roomNow)
	require.ErrorIs(t, err, datastore.ErrRoomNotFound)
}

func TestRoomRemovalAndRejoin(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	ctx := context.Background()
	roomWithMembers(t, db)
	_, err := db.AddRoomEnvelopes(ctx, roomID, ownerID, []datastore.RoomEnvelope{{ClientID: viewerID, Epoch: 1, Sealed: []byte("ev")}})
	require.NoError(t, err)

	ch, err := db.RemoveRoomMember(ctx, roomID, ownerID, viewerID, roomNow.Add(time.Minute))
	require.NoError(t, err)
	assert.ElementsMatch(t, []string{ownerID, editorID, viewerID}, ch.Members, "the removed member is told too")
	_, err = db.GetRoom(ctx, roomID, viewerID)
	require.ErrorIs(t, err, datastore.ErrRoomMemberRemoved)
	_, _, _, err = db.RoomChanges(ctx, roomID, viewerID, 0, 10)
	require.ErrorIs(t, err, datastore.ErrRoomMemberRemoved)
	_, err = db.RemoveRoomMember(ctx, roomID, ownerID, viewerID, roomNow)
	require.ErrorIs(t, err, datastore.ErrNotRoomMember)
	// The old link doesn't let it back; a newer invite does.
	require.ErrorIs(t, join(db, roomID, viewerID, "inv-v", "secret-v", roomNow.Add(2*time.Minute)), datastore.ErrRoomMemberRemoved)
	require.ErrorIs(t, join(db, roomID, viewerID, "", "", roomNow), datastore.ErrRoomMemberRemoved)
	_, err = db.CreateRoomInvite(ctx, roomID, ownerID, datastore.RoomInvite{ID: "new", SecretHash: secretHash("n"),
		Role: datastore.RoleViewer, Created: roomNow.Add(3 * time.Minute)})
	require.NoError(t, err)
	require.NoError(t, join(db, roomID, viewerID, "new", "n", roomNow.Add(4*time.Minute)))
	v, err := db.GetRoom(ctx, roomID, viewerID)
	require.NoError(t, err)
	assert.Empty(t, v.Envelopes, "its old envelopes went with the removal")

	// Leaving: anyone but the owner.
	_, err = db.RemoveRoomMember(ctx, roomID, editorID, editorID, roomNow)
	require.NoError(t, err)
	_, err = db.RemoveRoomMember(ctx, roomID, ownerID, ownerID, roomNow)
	require.ErrorIs(t, err, datastore.ErrOwnerMustTransfer)

	// Transfer, then the old owner (now an editor) may leave.
	_, err = db.TransferRoomOwner(ctx, roomID, ownerID, viewerID)
	require.NoError(t, err)
	v, err = db.GetRoom(ctx, roomID, viewerID)
	require.NoError(t, err)
	assert.Equal(t, viewerID, v.Room.Owner)
	assert.Equal(t, datastore.RoleOwner, v.Me.Role)
	v, err = db.GetRoom(ctx, roomID, ownerID)
	require.NoError(t, err)
	assert.Equal(t, datastore.RoleEditor, v.Me.Role)
	_, err = db.RemoveRoomMember(ctx, roomID, ownerID, ownerID, roomNow)
	require.NoError(t, err)
	// The removed editor can't be made owner.
	_, err = db.TransferRoomOwner(ctx, roomID, viewerID, editorID)
	require.ErrorIs(t, err, datastore.ErrNotRoomMember)
}

func TestRoomRecordsAndConflicts(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	ctx := context.Background()
	roomWithMembers(t, db)

	out, ch, err := db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{
		{ID: "a", Epoch: 1, Ciphertext: []byte("a1")},
		{ID: "b", Epoch: 1, Ciphertext: []byte("b1"), BaseSeq: i64(0)},
	})
	require.NoError(t, err)
	assert.Equal(t, int64(2), ch.Seq)
	assert.Equal(t, []int64{1, 2}, []int64{out[0].Seq, out[1].Seq})
	assert.ElementsMatch(t, []string{ownerID, editorID, viewerID}, ch.Members)

	// Last write wins without base_seq.
	_, _, err = db.CommitRoomRecords(ctx, roomID, ownerID, []datastore.RoomRecordWrite{{ID: "a", Epoch: 1, Ciphertext: []byte("a2")}})
	require.NoError(t, err)
	// A stale base_seq refuses the whole batch, with the current record.
	_, _, err = db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{
		{ID: "c", Epoch: 1, Ciphertext: []byte("c1")},
		{ID: "a", Epoch: 1, Ciphertext: []byte("a3"), BaseSeq: i64(1)},
		{ID: "z", Epoch: 1, Ciphertext: []byte("z1"), BaseSeq: i64(7)},
		{ID: "b", Epoch: 1, Ciphertext: []byte("b2"), BaseSeq: i64(0)},
	})
	var conflict *datastore.RoomConflictError
	require.ErrorAs(t, err, &conflict)
	require.Len(t, conflict.Current, 3)
	assert.Equal(t, datastore.RoomRecord{ID: "a", Seq: 3, Epoch: 1, Author: ownerID, Ciphertext: []byte("a2")}, conflict.Current[0])
	assert.Equal(t, datastore.RoomRecord{ID: "z"}, conflict.Current[1], "doesn't exist: seq 0")
	assert.Equal(t, int64(2), conflict.Current[2].Seq)
	recs, room, more, err := db.RoomChanges(ctx, roomID, viewerID, 0, 100)
	require.NoError(t, err)
	assert.False(t, more)
	assert.Equal(t, int64(3), room.Seq, "nothing of the refused batch")
	require.Len(t, recs, 2)
	assert.Equal(t, "b", recs[0].ID)
	assert.Equal(t, "a", recs[1].ID)

	// The right base_seq; a delete keeps a tombstone.
	_, ch, err = db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{
		{ID: "a", Epoch: 1, Ciphertext: []byte("a3"), BaseSeq: i64(3)},
		{ID: "b", Epoch: 1, Deleted: true, Ciphertext: []byte("ignored")},
	})
	require.NoError(t, err)
	assert.Equal(t, int64(5), ch.Seq)
	recs, _, _, err = db.RoomChanges(ctx, roomID, viewerID, 3, 100)
	require.NoError(t, err)
	require.Len(t, recs, 2)
	assert.Equal(t, datastore.RoomRecord{ID: "b", Seq: 5, Epoch: 1, Deleted: true, Author: editorID}, recs[1])

	// Pages.
	recs, _, more, err = db.RoomChanges(ctx, roomID, viewerID, 0, 1)
	require.NoError(t, err)
	assert.True(t, more)
	assert.Len(t, recs, 1)

	// Another epoch than the room's.
	_, _, err = db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{{ID: "a", Epoch: 2, Ciphertext: []byte("x")}})
	require.ErrorIs(t, err, datastore.ErrStaleEpoch)
}

func TestRoomRotation(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	ctx := context.Background()
	roomWithMembers(t, db)
	_, err := db.RemoveRoomMember(ctx, roomID, ownerID, viewerID, roomNow)
	require.NoError(t, err)

	all := map[string][]byte{ownerID: []byte("o2"), strings.ToUpper(editorID): []byte("e2")}
	_, err = db.RotateRoomEpoch(ctx, roomID, ownerID, 3, all)
	require.ErrorIs(t, err, datastore.ErrStaleEpoch, "must be the next epoch")
	_, err = db.RotateRoomEpoch(ctx, roomID, ownerID, 1, all)
	require.ErrorIs(t, err, datastore.ErrStaleEpoch)
	var mismatch *datastore.RoomEnvelopeError
	_, err = db.RotateRoomEpoch(ctx, roomID, ownerID, 2, map[string][]byte{ownerID: []byte("o2")})
	require.ErrorAs(t, err, &mismatch)
	assert.Equal(t, []string{editorID}, mismatch.Missing)
	_, err = db.RotateRoomEpoch(ctx, roomID, ownerID, 2,
		map[string][]byte{ownerID: []byte("o2"), editorID: []byte("e2"), viewerID: []byte("v2")})
	require.ErrorAs(t, err, &mismatch)
	assert.Equal(t, []string{viewerID}, mismatch.Unknown, "the removed member gets none")

	ch, err := db.RotateRoomEpoch(ctx, roomID, ownerID, 2, all)
	require.NoError(t, err)
	assert.ElementsMatch(t, []string{ownerID, editorID}, ch.Members)
	v, err := db.GetRoom(ctx, roomID, editorID)
	require.NoError(t, err)
	assert.Equal(t, int64(2), v.Room.Epoch)
	assert.Equal(t, []datastore.RoomEnvelope{{ClientID: editorID, Epoch: 2, Sealed: []byte("e2")}}, v.Envelopes)
	// Writes need the new epoch now.
	_, _, err = db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{{ID: "a", Epoch: 1, Ciphertext: []byte("x")}})
	require.ErrorIs(t, err, datastore.ErrStaleEpoch)
	_, _, err = db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{{ID: "a", Epoch: 2, Ciphertext: []byte("x")}})
	require.NoError(t, err)

	// Envelopes of past epochs for a member, not beyond the current one.
	_, err = db.AddRoomEnvelopes(ctx, roomID, ownerID, []datastore.RoomEnvelope{{ClientID: editorID, Epoch: 1, Sealed: []byte("e1")}})
	require.NoError(t, err)
	_, err = db.AddRoomEnvelopes(ctx, roomID, ownerID, []datastore.RoomEnvelope{{ClientID: editorID, Epoch: 3, Sealed: []byte("e3")}})
	require.ErrorIs(t, err, datastore.ErrStaleEpoch)
	_, err = db.AddRoomEnvelopes(ctx, roomID, ownerID, []datastore.RoomEnvelope{{ClientID: viewerID, Epoch: 2, Sealed: []byte("v2")}})
	require.ErrorIs(t, err, datastore.ErrNotRoomMember)
	v, err = db.GetRoom(ctx, roomID, editorID)
	require.NoError(t, err)
	assert.Len(t, v.Envelopes, 2)
}

func TestRoomEmailInvites(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	ctx := context.Background()
	newRoom(t, db, roomID, ownerID)
	nm := datastore.NewRoomMember{ClientID: otherID, Role: datastore.RoleEditor, IdentityPub: pub('k'),
		Envelopes: []datastore.RoomEnvelope{{Epoch: 1, Sealed: []byte("sealed")}}}
	_, err := db.AddRoomMembers(ctx, roomID, ownerID, []datastore.NewRoomMember{nm}, roomNow)
	require.ErrorIs(t, err, datastore.ErrNoIdentityKey)
	_, err = db.SetIdentityKey(ctx, otherID, pub('j'), roomNow)
	require.NoError(t, err)
	_, err = db.AddRoomMembers(ctx, roomID, ownerID, []datastore.NewRoomMember{nm}, roomNow)
	require.ErrorIs(t, err, datastore.ErrIdentityMismatch)
	_, err = db.SetIdentityKey(ctx, strings.ToUpper(otherID), pub('k'), roomNow)
	require.NoError(t, err)
	bad := nm
	bad.Envelopes = []datastore.RoomEnvelope{{Epoch: 2, Sealed: []byte("s")}}
	_, err = db.AddRoomMembers(ctx, roomID, ownerID, []datastore.NewRoomMember{bad}, roomNow)
	require.ErrorIs(t, err, datastore.ErrStaleEpoch)

	ch, err := db.AddRoomMembers(ctx, roomID, ownerID, []datastore.NewRoomMember{nm}, roomNow)
	require.NoError(t, err)
	assert.ElementsMatch(t, []string{ownerID, otherID}, ch.Members)
	_, err = db.AddRoomMembers(ctx, roomID, ownerID, []datastore.NewRoomMember{nm}, roomNow)
	require.ErrorIs(t, err, datastore.ErrAlreadyMember)

	// Pending: sees the room and its envelope, can't read records yet.
	v, err := db.GetRoom(ctx, roomID, otherID)
	require.NoError(t, err)
	assert.True(t, v.Me.Pending())
	assert.Equal(t, []byte("sealed"), v.Envelopes[0].Sealed)
	_, _, _, err = db.RoomChanges(ctx, roomID, otherID, 0, 10)
	require.ErrorIs(t, err, datastore.ErrRoomNotJoined)
	_, err = db.TransferRoomOwner(ctx, roomID, ownerID, otherID)
	require.ErrorIs(t, err, datastore.ErrRoomNotJoined)
	// Joining needs no invite, but its own key.
	_, _, err = db.JoinRoom(ctx, roomID, otherID, "", nil, pub('q'), nil, roomNow)
	require.ErrorIs(t, err, datastore.ErrIdentityMismatch)
	_, joined, err := db.JoinRoom(ctx, roomID, otherID, "", nil, pub('k'), []byte("prof"), roomNow)
	require.NoError(t, err)
	assert.True(t, joined)
	_, _, _, err = db.RoomChanges(ctx, roomID, otherID, 0, 10)
	require.NoError(t, err)
}

func TestRoomLimitsAndQuota(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 100)
	ctx := context.Background()
	newRoom(t, db, roomID, ownerID)
	// The owner's chain syncs in upper case (Chromium): the room's bytes
	// count there.
	require.NoError(t, db.TouchChain(ctx, strings.ToUpper(ownerID), roomNow))
	invite(t, db, roomID, "many", datastore.RoleEditor, "s", time.Time{}, nil)
	require.NoError(t, join(db, roomID, editorID, "many", "s", roomNow))

	_, _, err := db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{{ID: "a", Epoch: 1, Ciphertext: make([]byte, 60)}})
	require.NoError(t, err)
	n, err := db.ChainBytes(ctx, strings.ToUpper(ownerID))
	require.NoError(t, err)
	assert.Equal(t, int64(61), n, "ciphertext and ID, on the owner's chain")
	n, err = db.ChainBytes(ctx, strings.ToUpper(editorID))
	require.NoError(t, err)
	assert.Zero(t, n, "not the author's")
	_, _, err = db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{{ID: "b", Epoch: 1, Ciphertext: make([]byte, 60)}})
	require.ErrorIs(t, err, datastore.ErrQuotaExceeded)
	// Shrinking always works; a delete frees the bytes.
	_, _, err = db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{{ID: "a", Epoch: 1, Deleted: true}})
	require.NoError(t, err)
	n, err = db.ChainBytes(ctx, strings.ToUpper(ownerID))
	require.NoError(t, err)
	assert.Equal(t, int64(1), n)
	_, _, err = db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{{ID: "b", Epoch: 1, Ciphertext: make([]byte, 60)}})
	require.NoError(t, err)

	// Transfer moves the bytes (and checks the new owner's room for them).
	_, err = db.TransferRoomOwner(ctx, roomID, ownerID, editorID)
	require.NoError(t, err)
	n, err = db.ChainBytes(ctx, strings.ToUpper(ownerID))
	require.NoError(t, err)
	assert.Zero(t, n)
	n, err = db.ChainBytes(ctx, editorID)
	require.NoError(t, err)
	assert.Equal(t, int64(62), n)
	// Deleting the room gives them back.
	_, err = db.DeleteRoom(ctx, roomID, editorID)
	require.NoError(t, err)
	n, err = db.ChainBytes(ctx, editorID)
	require.NoError(t, err)
	assert.Zero(t, n)

	// 50 members at most.
	newRoom(t, db, "11111111111111111111111111111111", ownerID)
	_, err = db.CreateRoomInvite(ctx, "11111111111111111111111111111111", ownerID, datastore.RoomInvite{ID: "i",
		SecretHash: secretHash("s"), Role: datastore.RoleViewer, Created: roomNow})
	require.NoError(t, err)
	for i := 1; i < datastore.MaxRoomMembers; i++ {
		id := strings.Repeat("e", 62) + string("0123456789abcdef"[i/16]) + string("0123456789abcdef"[i%16])
		require.NoError(t, join(db, "11111111111111111111111111111111", id, "i", "s", roomNow), i)
	}
	require.ErrorIs(t, join(db, "11111111111111111111111111111111", otherID, "i", "s", roomNow), datastore.ErrRoomFull)
}

func TestRoomsGoWithTheChain(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	ctx := context.Background()
	roomWithMembers(t, db)
	const other = "22222222222222222222222222222222"
	newRoom(t, db, other, editorID)
	_, err := db.CreateRoomInvite(ctx, other, editorID, datastore.RoomInvite{ID: "i", SecretHash: secretHash("s"),
		Role: datastore.RoleViewer, Created: roomNow})
	require.NoError(t, err)
	require.NoError(t, join(db, other, ownerID, "i", "s", roomNow))
	_, _, err = db.CommitRoomRecords(ctx, other, editorID, []datastore.RoomRecordWrite{{ID: "a", Epoch: 1, Ciphertext: []byte("x")}})
	require.NoError(t, err)

	// The owner of roomID resets (Chromium's upper case): its room goes,
	// its membership in the other one too.
	_, err = db.ClearServerData(ctx, strings.ToUpper(ownerID))
	require.NoError(t, err)
	_, err = db.GetRoom(ctx, roomID, editorID)
	require.ErrorIs(t, err, datastore.ErrRoomNotFound)
	members, err := db.RoomMembersOf(ctx, other)
	require.NoError(t, err)
	assert.Equal(t, []string{editorID}, members)
	key, err := db.IdentityKey(ctx, ownerID)
	require.NoError(t, err)
	assert.Nil(t, key)
	rooms, err := db.ListRooms(ctx)
	require.NoError(t, err)
	require.Len(t, rooms, 1)

	// The other owner's chain goes idle: its room goes.
	require.NoError(t, db.TouchChain(ctx, editorID, roomNow.Add(-400*24*time.Hour)))
	_, err = db.ExpireIdleChains(ctx, roomNow.Add(-180*24*time.Hour))
	require.NoError(t, err)
	rooms, err = db.ListRooms(ctx)
	require.NoError(t, err)
	assert.Empty(t, rooms)
}

func TestDirectory(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 0)
	ctx := context.Background()
	const issuer = "https://idp.example.com"
	login := datastore.OIDCLogin{Issuer: issuer, Subject: "alice", Email: "Alice@Example.com", Name: "Alice"}
	_, err := db.SaveOIDCLogin(ctx, login, strings.ToUpper(ownerID), time.Time{}, roomNow)
	require.NoError(t, err)
	_, err = db.SaveOIDCLogin(ctx, login, editorID, roomNow.Add(time.Hour), roomNow)
	require.NoError(t, err)
	_, err = db.SaveOIDCLogin(ctx, login, viewerID, roomNow.Add(-time.Hour), roomNow) // Expired.
	require.NoError(t, err)
	_, err = db.SaveOIDCLogin(ctx, datastore.OIDCLogin{Issuer: "https://other", Subject: "a", Email: "alice@example.com"},
		otherID, time.Time{}, roomNow)
	require.NoError(t, err)
	for _, id := range []string{ownerID, viewerID, otherID} {
		_, err = db.SetIdentityKey(ctx, id, pub(id[0]), roomNow)
		require.NoError(t, err)
	}

	got, err := db.Directory(ctx, issuer, "alice@EXAMPLE.com", roomNow)
	require.NoError(t, err)
	// The editor's chain has no identity key, the viewer's session expired,
	// the other one is another issuer's.
	assert.Equal(t, []datastore.DirectoryEntry{{ClientID: ownerID, IdentityPub: pub(ownerID[0]), Name: "Alice",
		Email: "Alice@Example.com"}}, got)
	got, err = db.Directory(ctx, issuer, "bob@example.com", roomNow)
	require.NoError(t, err)
	assert.Empty(t, got)
}

func TestMigrateSchema4(t *testing.T) {
	path := filepath.Join(t.TempDir(), "v4.db")
	db := openSQLite(t, path, 0)
	ctx := context.Background()
	require.NoError(t, db.TouchChain(ctx, "old", time.Now()))
	require.NoError(t, db.Close())
	raw, err := sql.Open("sqlite", "file:"+path)
	require.NoError(t, err)
	_, err = raw.Exec(`DROP TABLE room_invites; DROP TABLE room_envelopes; DROP TABLE room_records;
		DROP TABLE room_members; DROP TABLE rooms; DROP TABLE identity_keys; PRAGMA user_version = 4;`)
	require.NoError(t, err)
	require.NoError(t, raw.Close())

	db = openSQLite(t, path, 0)
	last, err := db.LastActive(ctx, "old")
	require.NoError(t, err)
	assert.WithinDuration(t, time.Now(), last, time.Minute)
	newRoom(t, db, roomID, ownerID)
	rooms, err := db.ListRooms(ctx)
	require.NoError(t, err)
	assert.Len(t, rooms, 1)
	require.NoError(t, db.Close())

	raw, err = sql.Open("sqlite", "file:"+path)
	require.NoError(t, err)
	defer raw.Close()
	var version int
	require.NoError(t, raw.QueryRow(`PRAGMA user_version`).Scan(&version))
	assert.Equal(t, 5, version)
}

func TestRoomSuccessor(t *testing.T) {
	db := openSQLite(t, filepath.Join(t.TempDir(), "r.db"), 1000)
	ctx := context.Background()
	roomWithMembers(t, db)
	require.NoError(t, db.TouchChain(ctx, strings.ToUpper(ownerID), roomNow))
	_, _, err := db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{{ID: "a", Epoch: 1, Ciphertext: make([]byte, 99)}})
	require.NoError(t, err)
	newOwner := strings.Repeat("f0", 32)
	envs := []datastore.RoomEnvelope{{Epoch: 1, Sealed: []byte("for the new chain")}}

	// The current epoch's envelope is required; a member can't be its successor.
	_, err = db.MoveRoomMembership(ctx, roomID, ownerID, newOwner, pub('n'), nil, roomNow)
	require.ErrorIs(t, err, datastore.ErrStaleEpoch)
	_, err = db.MoveRoomMembership(ctx, roomID, ownerID, newOwner, pub('n'),
		[]datastore.RoomEnvelope{{Epoch: 2, Sealed: []byte("x")}}, roomNow)
	require.ErrorIs(t, err, datastore.ErrStaleEpoch)
	_, err = db.MoveRoomMembership(ctx, roomID, ownerID, editorID, pub('n'), envs, roomNow)
	require.ErrorIs(t, err, datastore.ErrAlreadyMember)
	_, err = db.MoveRoomMembership(ctx, roomID, otherID, newOwner, pub('n'), envs, roomNow)
	require.ErrorIs(t, err, datastore.ErrRoomNotFound)

	// The owner's reset: the new chain owns the room, with its bytes.
	ch, err := db.MoveRoomMembership(ctx, roomID, strings.ToUpper(ownerID), strings.ToUpper(newOwner), pub('n'), envs, roomNow)
	require.NoError(t, err)
	assert.ElementsMatch(t, []string{newOwner, editorID, viewerID, ownerID}, ch.Members, "the old chain is told too")
	v, err := db.GetRoom(ctx, roomID, newOwner)
	require.NoError(t, err)
	assert.Equal(t, newOwner, v.Room.Owner)
	assert.Equal(t, datastore.RoleOwner, v.Me.Role)
	assert.False(t, v.Me.Pending())
	assert.Equal(t, pub('n'), v.Me.IdentityPub)
	assert.Equal(t, []datastore.RoomEnvelope{{ClientID: newOwner, Epoch: 1, Sealed: []byte("for the new chain")}}, v.Envelopes)
	assert.Len(t, v.Members, 3)
	_, err = db.GetRoom(ctx, roomID, ownerID)
	require.ErrorIs(t, err, datastore.ErrRoomMemberRemoved)
	n, err := db.ChainBytes(ctx, strings.ToUpper(ownerID))
	require.NoError(t, err)
	assert.Zero(t, n)
	n, err = db.ChainBytes(ctx, strings.ToUpper(newOwner))
	require.NoError(t, err)
	assert.Equal(t, int64(100), n, "on the new chain, as its token sent it")
	key, err := db.IdentityKey(ctx, newOwner)
	require.NoError(t, err)
	assert.Equal(t, pub('n'), key)

	// Clearing the old chain no longer touches the room.
	_, err = db.ClearServerData(ctx, strings.ToUpper(ownerID))
	require.NoError(t, err)
	_, err = db.RotateRoomEpoch(ctx, roomID, newOwner, 2,
		map[string][]byte{newOwner: []byte("o"), editorID: []byte("e"), viewerID: []byte("v")})
	require.NoError(t, err, "the new owner owns it")

	// A member's reset keeps its role.
	newViewer := strings.Repeat("f1", 32)
	_, err = db.MoveRoomMembership(ctx, roomID, viewerID, newViewer, pub('w'),
		[]datastore.RoomEnvelope{{Epoch: 1, Sealed: []byte("1")}, {Epoch: 2, Sealed: []byte("2")}}, roomNow)
	require.NoError(t, err)
	v, err = db.GetRoom(ctx, roomID, newViewer)
	require.NoError(t, err)
	assert.Equal(t, datastore.RoleViewer, v.Me.Role)
	assert.Len(t, v.Envelopes, 2)
	_, _, err = db.CommitRoomRecords(ctx, roomID, newViewer, []datastore.RoomRecordWrite{{ID: "b", Epoch: 2, Ciphertext: []byte("x")}})
	require.ErrorIs(t, err, datastore.ErrRoomForbidden)

	// A pending member stays pending.
	pendingID := strings.Repeat("e5", 32)
	_, err = db.SetIdentityKey(ctx, pendingID, pub('p'), roomNow)
	require.NoError(t, err)
	_, err = db.AddRoomMembers(ctx, roomID, newOwner, []datastore.NewRoomMember{{ClientID: pendingID, Role: datastore.RoleEditor,
		IdentityPub: pub('p'), Envelopes: []datastore.RoomEnvelope{{Epoch: 2, Sealed: []byte("p")}}}}, roomNow)
	require.NoError(t, err)
	newPending := strings.Repeat("e6", 32)
	_, err = db.MoveRoomMembership(ctx, roomID, pendingID, newPending, pub('q'),
		[]datastore.RoomEnvelope{{Epoch: 2, Sealed: []byte("q")}}, roomNow)
	require.NoError(t, err)
	v, err = db.GetRoom(ctx, roomID, newPending)
	require.NoError(t, err)
	assert.True(t, v.Me.Pending())
	assert.Equal(t, datastore.RoleEditor, v.Me.Role)

	// A full quota on the new chain refuses the owner's move.
	big := strings.Repeat("f2", 32)
	_, err = db.CreateRoom(ctx, datastore.NewRoom{ID: "33333333333333333333333333333333", Owner: newOwner, Kind: "easel",
		IdentityPub: pub('n'), Envelope: []byte("e")}, roomNow)
	require.NoError(t, err)
	_, _, err = db.CommitRoomRecords(ctx, "33333333333333333333333333333333", newOwner,
		[]datastore.RoomRecordWrite{{ID: "x", Epoch: 1, Ciphertext: make([]byte, 500)}})
	require.NoError(t, err)
	_, _, err = db.CommitRoomRecords(ctx, roomID, editorID, []datastore.RoomRecordWrite{{ID: "c", Epoch: 2, Ciphertext: make([]byte, 300)}})
	require.NoError(t, err) // The new owner's chain: 100 + 501 + 301 bytes.
	_, err = db.CreateRoom(ctx, datastore.NewRoom{ID: "44444444444444444444444444444444", Owner: big, Kind: "easel",
		IdentityPub: pub('b'), Envelope: []byte("e")}, roomNow)
	require.NoError(t, err)
	_, _, err = db.CommitRoomRecords(ctx, "44444444444444444444444444444444", big,
		[]datastore.RoomRecordWrite{{ID: "x", Epoch: 1, Ciphertext: make([]byte, 600)}})
	require.NoError(t, err)
	_, err = db.MoveRoomMembership(ctx, roomID, newOwner, big, pub('b'),
		[]datastore.RoomEnvelope{{Epoch: 2, Sealed: []byte("b")}}, roomNow)
	require.ErrorIs(t, err, datastore.ErrQuotaExceeded)
}
