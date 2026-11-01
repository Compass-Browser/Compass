package server

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"time"

	"github.com/brave/go-sync/datastore"
)

// Healthcheck asks the server on this machine for /healthz (for container
// health checks: the image has no shell or curl). Returns the exit code.
func Healthcheck() int {
	cfg, err := LoadConfig()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	_, port, err := net.SplitHostPort(cfg.Addr)
	if err != nil {
		fmt.Fprintln(os.Stderr, "bad COMPASS_SYNC_ADDR:", err)
		return 1
	}
	client := http.Client{Timeout: 5 * time.Second}
	resp, err := client.Get("http://127.0.0.1:" + port + "/healthz")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		fmt.Fprintln(os.Stderr, "unhealthy:", resp.Status)
		return 1
	}
	return 0
}

// Backup writes a consistent copy of the database (COMPASS_SYNC_DB) to
// args[0], or to stdout for "-", safe while the server runs. Returns the
// exit code.
func Backup(args []string) int {
	if len(args) != 1 {
		fmt.Fprintln(os.Stderr, "usage: compass-sync-server backup FILE|-")
		return 2
	}
	if args[0] == "-" {
		return backupToStdout()
	}
	cfg, err := LoadConfig()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	db, err := datastore.NewSQLite(cfg.DBPath, datastore.SQLiteOptions{})
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	defer db.Close()
	if err = db.BackupTo(context.Background(), args[0]); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Fprintln(os.Stderr, "backed up", cfg.DBPath, "to", args[0])
	return 0
}

// backupToStdout backs up to a temporary file next to the database, copies
// it to stdout and deletes it (the image has no shell to clean up with).
func backupToStdout() int {
	cfg, err := LoadConfig()
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	dir, err := os.MkdirTemp(filepath.Dir(cfg.DBPath), ".backup-")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	defer os.RemoveAll(dir)
	tmp := filepath.Join(dir, "backup.db")

	if code := Backup([]string{tmp}); code != 0 { // Reports on stderr.
		return code
	}
	f, err := os.Open(tmp)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	defer f.Close()
	if _, err = io.Copy(os.Stdout, f); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	return 0
}
