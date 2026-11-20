package server

// Compass: the admin commands for company sign-in (README, "Company sign-in
// (OIDC)"), run against COMPASS_SYNC_DB, safe while the server runs (it
// sees a change within its gate cache's 10 s).

import (
	"context"
	"fmt"
	"io"
	"os"
	"text/tabwriter"
	"time"

	"github.com/brave/go-sync/datastore"
)

const usersUsage = `usage: compass-sync-server users list
       compass-sync-server users revoke EMAIL|SUBJECT [--delete-data]
       compass-sync-server users restore EMAIL|SUBJECT
       compass-sync-server chains list`

func adminDB(stderr io.Writer) (*datastore.SQLite, bool) {
	path := os.Getenv("COMPASS_SYNC_DB")
	if path == "" {
		path = "/data/compass-sync.db"
	}
	db, err := datastore.NewSQLite(path, datastore.SQLiteOptions{})
	if err != nil {
		fmt.Fprintln(stderr, err)
		return nil, false
	}
	return db, true
}

func fmtTime(t time.Time) string {
	if t.IsZero() {
		return "-"
	}
	return t.UTC().Format("2006-01-02 15:04Z")
}

// Users runs "users list|revoke|restore". Returns the exit code.
func Users(args []string, stdout, stderr io.Writer) int {
	if len(args) == 0 {
		fmt.Fprintln(stderr, usersUsage)
		return 2
	}
	switch {
	case args[0] == "list" && len(args) == 1:
	case args[0] == "revoke" && (len(args) == 2 || (len(args) == 3 && args[2] == "--delete-data")):
	case args[0] == "restore" && len(args) == 2:
	default:
		fmt.Fprintln(stderr, usersUsage)
		return 2
	}
	db, ok := adminDB(stderr)
	if !ok {
		return 1
	}
	defer db.Close()
	ctx := context.Background()
	if args[0] == "list" {
		users, err := db.ListOIDCUsers(ctx)
		if err != nil {
			fmt.Fprintln(stderr, err)
			return 1
		}
		tw := tabwriter.NewWriter(stdout, 0, 4, 2, ' ', 0)
		fmt.Fprintln(tw, "EMAIL\tNAME\tSUBJECT\tCHAINS\tLAST LOGIN\tSTATUS")
		for _, u := range users {
			status := "active"
			if u.Revoked {
				status = "revoked"
			}
			fmt.Fprintf(tw, "%s\t%s\t%s\t%d\t%s\t%s\n", u.Email, u.Name, u.Subject, u.Chains,
				fmtTime(u.LastLogin), status)
		}
		_ = tw.Flush()
		return 0
	}
	users, err := db.FindOIDCUsers(ctx, args[1])
	if err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}
	if len(users) == 0 {
		fmt.Fprintln(stderr, "no user with email or subject", args[1])
		return 1
	}
	for _, u := range users {
		if args[0] == "restore" {
			if err = db.RestoreOIDCUser(ctx, u.ID); err != nil {
				fmt.Fprintln(stderr, err)
				return 1
			}
			fmt.Fprintf(stdout, "restored %s (%s): they can sign in again\n", u.Email, u.Subject)
			continue
		}
		chains, err := db.EndOIDCUserSessions(ctx, u.ID, true)
		if err != nil {
			fmt.Fprintln(stderr, err)
			return 1
		}
		fmt.Fprintf(stdout, "revoked %s (%s): %d chain(s) signed out\n", u.Email, u.Subject, len(chains))
		if len(args) == 3 {
			for _, c := range chains {
				if err = deleteChainData(ctx, db, c); err != nil {
					fmt.Fprintln(stderr, err)
					return 1
				}
				fmt.Fprintln(stdout, "  deleted the data of chain", c)
			}
		}
	}
	return 0
}

// deleteChainData clears and disables chain |id| (lower-case), under every
// case its devices sent it in, as an idle chain is (ExpireIdleChains).
func deleteChainData(ctx context.Context, db *datastore.SQLite, id string) error {
	stored, err := db.StoredChainIDs(ctx, id)
	if err != nil {
		return err
	}
	if len(stored) == 0 {
		stored = []string{id}
	}
	for _, s := range stored {
		if err = db.DisableSyncChain(ctx, s); err != nil {
			return err
		}
		if _, err = db.ClearServerData(ctx, s); err != nil {
			return err
		}
	}
	return nil
}

// Chains runs "chains list". Returns the exit code.
func Chains(args []string, stdout, stderr io.Writer) int {
	if len(args) != 1 || args[0] != "list" {
		fmt.Fprintln(stderr, usersUsage)
		return 2
	}
	db, ok := adminDB(stderr)
	if !ok {
		return 1
	}
	defer db.Close()
	bindings, err := db.ListChainBindings(context.Background())
	if err != nil {
		fmt.Fprintln(stderr, err)
		return 1
	}
	tw := tabwriter.NewWriter(stdout, 0, 4, 2, ' ', 0)
	fmt.Fprintln(tw, "CHAIN\tEMAIL\tLAST ACTIVE\tSESSION EXPIRES")
	for _, b := range bindings {
		exp := fmtTime(b.SessionExpires)
		if b.SessionExpires.IsZero() {
			exp = "never (revalidated)"
		}
		fmt.Fprintf(tw, "%s\t%s\t%s\t%s\n", b.ClientID, b.User.Email, fmtTime(b.LastActive), exp)
	}
	_ = tw.Flush()
	return 0
}
