package server_test

// History as Compass sends it (sync.md, "History"):
// HISTORY and HISTORY_DELETE_DIRECTIVES encrypted with the chain's custom
// passphrase, so the server only sees EncryptedData plus an empty field
// naming the type. It must store, update and serve them like any other
// type, with go-sync's history rules (client tag hash as the ID, update by
// tag) still working on opaque data.

import (
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
	"google.golang.org/protobuf/proto"

	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
)

const (
	historyTypeID         = 963985
	deleteDirectiveTypeID = 150251
)

// encryptedHistory is a HISTORY entity as Chromium commits it under a custom
// passphrase: EncryptedData and an empty HistorySpecifics.
func encryptedHistory(id, tagHash, blob string, version int64) *sync_pb.SyncEntity {
	return &sync_pb.SyncEntity{
		IdString: ptr.String(id), Version: ptr.Int64(version), Name: ptr.String("encrypted"),
		ClientTagHash: ptr.String(tagHash), Deleted: ptr.Bool(false),
		Specifics: &sync_pb.EntitySpecifics{
			Encrypted:        &sync_pb.EncryptedData{KeyName: ptr.String("k"), Blob: ptr.String(blob)},
			SpecificsVariant: &sync_pb.EntitySpecifics_History{History: &sync_pb.HistorySpecifics{}},
		},
	}
}

func encryptedDirective(id, tagHash, blob string, version int64) *sync_pb.SyncEntity {
	return &sync_pb.SyncEntity{
		IdString: ptr.String(id), Version: ptr.Int64(version), Name: ptr.String("encrypted"),
		ClientTagHash: ptr.String(tagHash), Deleted: ptr.Bool(false),
		Specifics: &sync_pb.EntitySpecifics{
			Encrypted: &sync_pb.EncryptedData{KeyName: ptr.String("k"), Blob: ptr.String(blob)},
			SpecificsVariant: &sync_pb.EntitySpecifics_HistoryDeleteDirective{
				HistoryDeleteDirective: &sync_pb.HistoryDeleteDirectiveSpecifics{}},
		},
	}
}

func findByClientTag(entries []*sync_pb.SyncEntity, tagHash string) *sync_pb.SyncEntity {
	for _, e := range entries {
		if e.GetClientTagHash() == tagHash {
			return e
		}
	}
	return nil
}

func TestEndToEndEncryptedHistory(t *testing.T) {
	url := serverURL(t)
	seed := randomSeed(t)
	a := newDevice(t, url, seed, "cache-guid-a")
	b := newDevice(t, url, seed, "cache-guid-b")
	a.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID)

	// A visits a page and deletes another: one history entity, one directive.
	rsps := a.commit(
		encryptedHistory("local-h-1", "history-tag-1", "visit ciphertext", 0),
		encryptedDirective("local-dd-1", "directive-tag-1", "directive ciphertext", 0))
	require.Len(t, rsps, 2)
	for _, r := range rsps {
		require.Equal(t, sync_pb.CommitResponse_SUCCESS, r.GetResponseType(), r.GetErrorMessage())
	}
	// go-sync keys history by the client tag hash.
	assert.Equal(t, "history-tag-1", rsps[0].GetIdString())
	historyVersion := rsps[0].GetVersion()
	directiveID, directiveVersion := rsps[1].GetIdString(), rsps[1].GetVersion()

	// B joins and gets both, ciphertext unchanged.
	b.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID)
	got := b.getUpdates(sync_pb.SyncEnums_GU_TRIGGER, historyTypeID, deleteDirectiveTypeID)
	visit := findByClientTag(got, "history-tag-1")
	require.NotNil(t, visit)
	assert.Equal(t, "visit ciphertext", visit.GetSpecifics().GetEncrypted().GetBlob())
	assert.NotNil(t, visit.GetSpecifics().GetHistory(), "the type's field survives")
	assert.Equal(t, "cache-guid-a", visit.GetOriginatorCacheGuid())
	directive := findByClientTag(got, "directive-tag-1")
	require.NotNil(t, directive)
	assert.Equal(t, "directive ciphertext", directive.GetSpecifics().GetEncrypted().GetBlob())
	assert.NotNil(t, directive.GetSpecifics().GetHistoryDeleteDirective())

	// A updates the visit (its duration became known): same tag, an update,
	// not a second entity.
	urs := a.commit(encryptedHistory("history-tag-1", "history-tag-1", "visit ciphertext 2", historyVersion))
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, urs[0].GetResponseType(), urs[0].GetErrorMessage())
	got = b.getUpdates(sync_pb.SyncEnums_PERIODIC, historyTypeID)
	require.Len(t, got, 1)
	assert.Equal(t, "visit ciphertext 2", got[0].GetSpecifics().GetEncrypted().GetBlob())

	// A applied the directive and drops it (delete directives act once); B
	// gets the tombstone.
	tomb := encryptedDirective(directiveID, "directive-tag-1", "directive ciphertext", directiveVersion)
	tomb.Deleted = ptr.Bool(true)
	drs := a.commit(tomb)
	require.Equal(t, sync_pb.CommitResponse_SUCCESS, drs[0].GetResponseType(), drs[0].GetErrorMessage())
	got = b.getUpdates(sync_pb.SyncEnums_PERIODIC, deleteDirectiveTypeID)
	require.Len(t, got, 1)
	assert.True(t, got[0].GetDeleted())

	// The visit is still there for a device that joins later.
	c := newDevice(t, url, seed, "cache-guid-c")
	c.getUpdates(sync_pb.SyncEnums_NEW_CLIENT, nigoriTypeID)
	got = c.getUpdates(sync_pb.SyncEnums_GU_TRIGGER, historyTypeID)
	visit = findByClientTag(got, "history-tag-1")
	require.NotNil(t, visit)
	raw, err := proto.Marshal(visit.GetSpecifics())
	require.NoError(t, err)
	want, err := proto.Marshal(encryptedHistory("", "", "visit ciphertext 2", 0).GetSpecifics())
	require.NoError(t, err)
	assert.Equal(t, want, raw)
}
