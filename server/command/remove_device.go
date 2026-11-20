package command

import (
	"context"
	"errors"
	"fmt"

	"github.com/brave/go-sync/cache"
	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/ptr"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
)

// Compass: removing a device from a chain (docs/design/sync.md, "Identity").

// TaggedEntityFinder finds the entity a client tag hash belongs to
// (datastore.SQLite).
type TaggedEntityFinder interface {
	EntityByClientTag(ctx context.Context, clientID string, dataType int, tagHash string) (*datastore.SyncEntity, error)
}

// deleteDeviceInfoAttempts bounds the retries when the device commits its
// entry at the same time (a version conflict).
const deleteDeviceInfoAttempts = 3

// DeleteDeviceInfo deletes the DEVICE_INFO entity of device |cacheGUID| in
// chain |clientID| (as the device's token gave it) as a client's commit of
// the deletion would: through handleCommitRequest, so the entity gets a new
// version and mtime and deleted = true, its tag is freed, the chain's item
// count and bytes go down and the type's cached mtime moves (other devices
// get the tombstone with their next GetUpdates). Returns false if there was
// no live entity.
func DeleteDeviceInfo(
	ctx context.Context,
	c *cache.Cache,
	db datastore.Datastore,
	find TaggedEntityFinder,
	clientID, cacheGUID string,
) (bool, error) {
	tag := datastore.ClientTagHash(deviceInfoTypeID, datastore.DeviceInfoTag(cacheGUID))
	for range deleteDeviceInfoAttempts {
		e, err := find.EntityByClientTag(ctx, clientID, deviceInfoTypeID, tag)
		if err != nil {
			return false, err
		}
		if e == nil || e.Version == nil || (e.Deleted != nil && *e.Deleted) {
			return false, nil
		}
		msg := &sync_pb.CommitMessage{Entries: []*sync_pb.SyncEntity{{
			IdString:      ptr.String(e.ID),
			Version:       ptr.Int64(*e.Version),
			Deleted:       ptr.Bool(true),
			ClientTagHash: ptr.String(tag),
			// A deletion carries the type's empty specifics, as Chromium's do.
			Specifics: &sync_pb.EntitySpecifics{SpecificsVariant: &sync_pb.EntitySpecifics_DeviceInfo{
				DeviceInfo: &sync_pb.DeviceInfoSpecifics{}}},
		}}}
		rsp := &sync_pb.CommitResponse{}
		errCode, err := handleCommitRequest(ctx, c, msg, rsp, db, clientID)
		if err != nil {
			return false, err
		}
		if errCode == nil || *errCode != sync_pb.SyncEnums_SUCCESS {
			return false, fmt.Errorf("deleting device info: %v", errCode)
		}
		entry := rsp.Entryresponse[0]
		switch entry.GetResponseType() {
		case sync_pb.CommitResponse_SUCCESS:
			return true, nil
		case sync_pb.CommitResponse_CONFLICT:
			continue // Committed meanwhile: read it again.
		default:
			return false, fmt.Errorf("deleting device info: %v %s", entry.GetResponseType(), entry.GetErrorMessage())
		}
	}
	return false, errors.New("deleting device info: the entry kept changing")
}
