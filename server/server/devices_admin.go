package server

// Compass: the admin commands for a chain's devices (README, "Devices"),
// run against COMPASS_SYNC_DB, safe while the server runs.

import (
	"context"
	"fmt"
	"io"
	"sort"
	"strings"
	"text/tabwriter"
	"time"

	"google.golang.org/protobuf/proto"

	"github.com/brave/go-sync/cache"
	"github.com/brave/go-sync/command"
	"github.com/brave/go-sync/datastore"
	"github.com/brave/go-sync/devices"
	"github.com/brave/go-sync/schema/protobuf/sync_pb"
)

const devicesUsage = `usage: compass-sync-server devices list CHAIN_ID
       compass-sync-server devices remove CHAIN_ID CACHE_GUID`

// deviceRow is one line of "devices list".
type deviceRow struct {
	guid, name, os string
	updated        time.Time
	status         string
}

// osName names a device's OS from its DEVICE_INFO.
func osName(d *sync_pb.DeviceInfoSpecifics) string {
	if d.OsType != nil && d.GetOsType() != sync_pb.SyncEnums_OS_TYPE_UNSPECIFIED {
		return strings.ToLower(strings.TrimPrefix(d.GetOsType().String(), "OS_TYPE_"))
	}
	//nolint:staticcheck // SA1019: device_type is what older clients set.
	if d.DeviceType != nil && d.GetDeviceType() != sync_pb.SyncEnums_TYPE_UNSET {
		return strings.ToLower(strings.TrimPrefix(d.GetDeviceType().String(), "TYPE_")) //nolint:staticcheck // SA1019
	}
	return "-"
}

// chainDevices lists the devices of chain |id| (lower-case): the live
// DEVICE_INFO entries (their specifics are not encrypted) and the removed
// cache GUIDs.
func chainDevices(ctx context.Context, db *datastore.SQLite, stored []string, id string) ([]deviceRow, error) {
	removed, err := db.RemovedDevices(ctx, id)
	if err != nil {
		return nil, err
	}
	removedAt := map[string]time.Time{}
	for _, r := range removed {
		removedAt[r.CacheGUID] = r.RemovedAt
	}
	byGUID := map[string]deviceRow{}
	for _, s := range stored {
		entities, err := db.EntitiesOfType(ctx, s, datastore.DeviceInfoTypeID)
		if err != nil {
			return nil, err
		}
		for _, e := range entities {
			if e.Deleted != nil && *e.Deleted {
				continue // A tombstone's specifics are empty: no GUID to show.
			}
			spec := &sync_pb.EntitySpecifics{}
			row := deviceRow{guid: "?", name: "?", os: "?", status: "active"}
			if e.Mtime != nil {
				row.updated = time.UnixMilli(*e.Mtime)
			}
			if proto.Unmarshal(e.Specifics, spec) == nil && spec.GetDeviceInfo() != nil {
				d := spec.GetDeviceInfo()
				row.guid, row.name, row.os = d.GetCacheGuid(), d.GetClientName(), osName(d)
				if d.LastUpdatedTimestamp != nil {
					row.updated = time.UnixMilli(d.GetLastUpdatedTimestamp())
				}
			}
			if row.guid == "?" {
				row.guid = "? (entity " + e.ID + ")"
			}
			byGUID[row.guid] = row
		}
	}
	for guid, at := range removedAt {
		row, ok := byGUID[guid]
		if !ok {
			row = deviceRow{guid: guid, name: "-", os: "-"}
		}
		row.status = "removed " + fmtTime(at)
		byGUID[guid] = row
	}
	rows := make([]deviceRow, 0, len(byGUID))
	for _, r := range byGUID {
		rows = append(rows, r)
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].guid < rows[j].guid })
	return rows, nil
}

// Devices runs "devices list|remove". Returns the exit code.
func Devices(args []string, stdout, stderr io.Writer) int {
	switch {
	case len(args) == 2 && args[0] == "list":
	case len(args) == 3 && args[0] == "remove":
		if args[2] == "" || len(args[2]) > devices.MaxCacheGUID {
			fmt.Fprintln(stderr, "invalid cache GUID")
			return 2
		}
	default:
		fmt.Fprintln(stderr, devicesUsage)
		return 2
	}
	id := strings.ToLower(strings.TrimSpace(args[1]))
	db, ok := adminDB(stderr)
	if !ok {
		return 1
	}
	defer db.Close()
	ctx := context.Background()
	stored, err := db.StoredChainIDs(ctx, id)
	if err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}
	if args[0] == "list" {
		rows, err := chainDevices(ctx, db, stored, id)
		if err != nil {
			fmt.Fprintln(stderr, err)
			return 1
		}
		tw := tabwriter.NewWriter(stdout, 0, 4, 2, ' ', 0)
		fmt.Fprintln(tw, "CACHE GUID\tNAME\tOS\tLAST UPDATED\tSTATUS")
		for _, r := range rows {
			fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\n", r.guid, r.name, r.os, fmtTime(r.updated), r.status)
		}
		_ = tw.Flush()
		return 0
	}
	if len(stored) == 0 {
		fmt.Fprintln(stderr, "no chain", id)
		return 1
	}
	guid := args[2]
	if err = db.RemoveDevice(ctx, id, guid, time.Now()); err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}
	// This process has its own cache: the server's poll cache learns of the
	// change within its 10 minutes; other GetUpdates see it at once.
	c := cache.NewCache(cache.NewMemoryClient())
	deleted := false
	for _, s := range stored {
		d, err := command.DeleteDeviceInfo(ctx, c, db, db, s, guid)
		if err != nil {
			fmt.Fprintln(stderr, err)
			return 1
		}
		deleted = deleted || d
	}
	if deleted {
		fmt.Fprintf(stdout, "removed %s from chain %s: its entry is deleted and it can't sync with this chain again\n", guid, id)
	} else {
		fmt.Fprintf(stdout, "removed %s from chain %s (it had no entry): it can't sync with this chain again\n", guid, id)
	}
	return 0
}
