package server

// Compass: the admin commands for rooms (README, "Rooms"), run against
// COMPASS_SYNC_DB, safe while the server runs. A room deleted here isn't
// announced on the running server's change streams: members learn it at
// their next request (404).

import (
	"context"
	"errors"
	"fmt"
	"io"
	"text/tabwriter"

	"github.com/brave/go-sync/datastore"
)

const roomsUsage = `usage: compass-sync-server rooms list
       compass-sync-server rooms show ROOM_ID
       compass-sync-server rooms delete ROOM_ID`

// Rooms runs "rooms list|show|delete". Returns the exit code.
func Rooms(args []string, stdout, stderr io.Writer) int {
	switch {
	case len(args) == 1 && args[0] == "list":
	case len(args) == 2 && (args[0] == "show" || args[0] == "delete"):
	default:
		fmt.Fprintln(stderr, roomsUsage)
		return 2
	}
	db, ok := adminDB(stderr)
	if !ok {
		return 1
	}
	defer db.Close()
	ctx := context.Background()
	switch args[0] {
	case "list":
		rooms, err := db.ListRooms(ctx)
		if err != nil {
			fmt.Fprintln(stderr, err)
			return 1
		}
		tw := tabwriter.NewWriter(stdout, 0, 4, 2, ' ', 0)
		fmt.Fprintln(tw, "ROOM\tKIND\tOWNER\tMEMBERS\tEPOCH\tSEQ\tBYTES\tCREATED")
		for _, r := range rooms {
			fmt.Fprintf(tw, "%s\t%s\t%s\t%d\t%d\t%d\t%d\t%s\n", r.ID, r.Kind, r.Owner, r.Members, r.Epoch,
				r.Seq, r.Bytes, fmtTime(r.Created))
		}
		_ = tw.Flush()
		return 0
	case "show":
		d, err := db.AdminRoom(ctx, args[1])
		if errors.Is(err, datastore.ErrRoomNotFound) {
			fmt.Fprintln(stderr, "no room", args[1])
			return 1
		}
		if err != nil {
			fmt.Fprintln(stderr, err)
			return 1
		}
		r := d.Room
		fmt.Fprintf(stdout, "room %s (%s), created %s\nowner %s\nepoch %d, seq %d, %d records, %d bytes (counted for chain %s)\n\n",
			r.ID, r.Kind, fmtTime(r.Created), r.Owner, r.Epoch, r.Seq, d.Records, r.Bytes, r.QuotaChain)
		tw := tabwriter.NewWriter(stdout, 0, 4, 2, ' ', 0)
		fmt.Fprintln(tw, "MEMBER\tROLE\tSTATUS\tINVITED\tJOINED")
		for _, m := range d.Members {
			status := "member"
			switch {
			case !m.Removed.IsZero():
				status = "removed " + fmtTime(m.Removed)
			case m.Pending():
				status = "pending"
			}
			fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\n", m.ClientID, m.Role, status, fmtTime(m.Invited), fmtTime(m.Joined))
		}
		_ = tw.Flush()
		if len(d.Invites) > 0 {
			fmt.Fprintln(stdout)
			tw = tabwriter.NewWriter(stdout, 0, 4, 2, ' ', 0)
			fmt.Fprintln(tw, "INVITE\tROLE\tCREATED\tEXPIRES\tUSES LEFT\tSTATUS")
			for _, inv := range d.Invites {
				expires, uses, status := "never", "unlimited", "active"
				if !inv.Expires.IsZero() {
					expires = fmtTime(inv.Expires)
				}
				if inv.UsesLeft != nil {
					uses = fmt.Sprint(*inv.UsesLeft)
				}
				if inv.Revoked {
					status = "revoked"
				}
				fmt.Fprintf(tw, "%s\t%s\t%s\t%s\t%s\t%s\n", inv.ID, inv.Role, fmtTime(inv.Created), expires, uses, status)
			}
			_ = tw.Flush()
		}
		return 0
	default: // delete
		ch, err := db.DeleteRoom(ctx, args[1], "")
		if errors.Is(err, datastore.ErrRoomNotFound) {
			fmt.Fprintln(stderr, "no room", args[1])
			return 1
		}
		if err != nil {
			fmt.Fprintln(stderr, err)
			return 1
		}
		fmt.Fprintf(stdout, "deleted room %s (%d member(s)): its records and keys are gone\n",
			args[1], len(ch.Members))
		return 0
	}
}
