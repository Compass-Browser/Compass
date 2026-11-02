package command_test

// A create (version 0) over a deleted entity's client tag revives it under
// its server ID (Chromium's semantics); over a live one it's a CONFLICT.

import (
	"context"
	"encoding/binary"

	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	"github.com/brave/go-sync/command"
	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
)

const (
	passwordType      int32 = 45873
	compassSpacesType int32 = 970000
)

func passwordSpecifics(blob string) *sync_pb.EntitySpecifics {
	return &sync_pb.EntitySpecifics{SpecificsVariant: &sync_pb.EntitySpecifics_Password{
		Password: &sync_pb.PasswordSpecifics{
			Encrypted: &sync_pb.EncryptedData{KeyName: ptr.String("k"), Blob: ptr.String(blob)},
		},
	}}
}

// compassSpacesSpecifics is encrypted data (field 1) and an empty field
// 970000, as an encrypted COMPASS_SPACES entity arrives at a server whose
// protos don't have the type.
func (suite *CommandTestSuite) compassSpacesSpecifics(blob string) *sync_pb.EntitySpecifics {
	enc, err := proto.Marshal(&sync_pb.EncryptedData{KeyName: ptr.String("k"), Blob: ptr.String(blob)})
	suite.Require().NoError(err)
	var raw []byte
	raw = protowire.AppendTag(raw, 1, protowire.BytesType)
	raw = protowire.AppendBytes(raw, enc)
	raw = protowire.AppendTag(raw, protowire.Number(compassSpacesType), protowire.BytesType)
	raw = protowire.AppendBytes(raw, nil)
	s := &sync_pb.EntitySpecifics{}
	suite.Require().NoError(proto.Unmarshal(raw, s))
	return s
}

func (suite *CommandTestSuite) commitOne(e *sync_pb.SyncEntity) *sync_pb.CommitResponse_EntryResponse {
	rsp := &sync_pb.ClientToServerResponse{}
	suite.Require().NoError(command.HandleClientToServerMessage(context.Background(), suite.cache,
		getClientToServerCommitMsg([]*sync_pb.SyncEntity{e}), rsp, suite.db, clientID))
	suite.Require().Equal(sync_pb.SyncEnums_SUCCESS, rsp.GetErrorCode())
	suite.Require().Len(rsp.Commit.Entryresponse, 1)
	return rsp.Commit.Entryresponse[0]
}

func (suite *CommandTestSuite) updatesSince(dataType int32, token int64) []*sync_pb.SyncEntity {
	tok := make([]byte, binary.MaxVarintLen64)
	binary.PutVarint(tok, token)
	marker := []*sync_pb.DataTypeProgressMarker{{DataTypeId: ptr.Int32(dataType), Token: tok}}
	rsp := &sync_pb.ClientToServerResponse{}
	suite.Require().NoError(command.HandleClientToServerMessage(context.Background(), suite.cache,
		getClientToServerGUMsg(marker, sync_pb.SyncEnums_PERIODIC, true, nil), rsp, suite.db, clientID))
	suite.Require().Equal(sync_pb.SyncEnums_SUCCESS, rsp.GetErrorCode())
	return rsp.GetUpdates.Entries
}

func (suite *CommandTestSuite) itemCount() int {
	counts, err := suite.db.GetClientItemCount(context.Background(), clientID)
	suite.Require().NoError(err)
	return counts.ItemCount
}

func taggedCommit(id, tag string, version int64, deleted bool, specifics *sync_pb.EntitySpecifics) *sync_pb.SyncEntity {
	e := getCommitEntity(id, version, deleted, specifics)
	e.ClientTagHash = ptr.String(tag)
	return e
}

func (suite *CommandTestSuite) TestHandleClientToServerMessage_CreateOverTombstoneRevives() {
	for _, tc := range []struct {
		name      string
		dataType  int32
		specifics func(string) *sync_pb.EntitySpecifics
	}{
		{"password", passwordType, passwordSpecifics},
		{"compass spaces", compassSpacesType, suite.compassSpacesSpecifics},
	} {
		suite.Run(tc.name, func() {
			suite.SetupTest()
			defer suite.TearDownTest()

			created := suite.commitOne(taggedCommit("local-1", "tag", 0, false, tc.specifics("v1")))
			suite.Require().Equal(sync_pb.CommitResponse_SUCCESS, created.GetResponseType())
			serverID := created.GetIdString()
			suite.Equal(1, suite.itemCount())

			deleted := suite.commitOne(taggedCommit(serverID, "tag", created.GetVersion(), true, tc.specifics("")))
			suite.Require().Equal(sync_pb.CommitResponse_SUCCESS, deleted.GetResponseType())
			suite.Equal(0, suite.itemCount())

			// Saved again (same site and username; a space put back under
			// its old ID): the same entity, a newer version.
			again := suite.commitOne(taggedCommit("local-2", "tag", 0, false, tc.specifics("v2")))
			suite.Require().Equal(sync_pb.CommitResponse_SUCCESS, again.GetResponseType(), again.GetErrorMessage())
			suite.Equal(serverID, again.GetIdString())
			suite.Greater(again.GetVersion(), deleted.GetVersion())
			suite.Equal(1, suite.itemCount(), "a revived entity counts again")

			// Another device that saw the delete gets it back, undeleted.
			got := suite.updatesSince(tc.dataType, deleted.GetMtime())
			suite.Require().Len(got, 1)
			suite.Equal(serverID, got[0].GetIdString())
			suite.False(got[0].GetDeleted())
			suite.Equal(again.GetVersion(), got[0].GetVersion())
			suite.Equal("local-2", got[0].GetOriginatorClientItemId())
			suite.True(proto.Equal(tc.specifics("v2"), got[0].GetSpecifics()))

			// A create over the now live tag replaces it, under its ID, and
			// the chain still counts one.
			dup := suite.commitOne(taggedCommit("local-3", "tag", 0, false, tc.specifics("v3")))
			suite.Equal(sync_pb.CommitResponse_SUCCESS, dup.GetResponseType(), dup.GetErrorMessage())
			suite.Equal(serverID, dup.GetIdString())
			suite.Equal(1, suite.itemCount())
		})
	}
}

func (suite *CommandTestSuite) TestHandleClientToServerMessage_CreateOverLiveTagReplaces() {
	// A device that lost track of an entity another created (the icon of one
	// host) creates it again: it replaces it, instead of a CONFLICT that
	// would keep the device retrying for good.
	created := suite.commitOne(taggedCommit("local-1", "tag", 0, false, passwordSpecifics("v1")))
	suite.Require().Equal(sync_pb.CommitResponse_SUCCESS, created.GetResponseType())
	dup := suite.commitOne(taggedCommit("local-2", "tag", 0, false, passwordSpecifics("v2")))
	suite.Equal(sync_pb.CommitResponse_SUCCESS, dup.GetResponseType(), dup.GetErrorMessage())
	suite.Equal(created.GetIdString(), dup.GetIdString())
	suite.Greater(dup.GetVersion(), created.GetVersion())
	suite.Equal(1, suite.itemCount())
}
