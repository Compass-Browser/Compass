package server

// Compass (slice 7): periodic backups of the database, made by the server
// itself (VACUUM INTO, consistent while it runs) into a directory next to
// the database, keeping the newest few. See server/README.md, "Backups".

import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"time"

	"github.com/rs/zerolog/log"
)

const (
	backupPrefix = "compass-sync-"
	backupSuffix = ".db"
	// backupTimeLayout sorts by name in time order.
	backupTimeLayout = "20060102T150405Z"
)

// BackupConfig says where and how often to back up.
type BackupConfig struct {
	Dir      string        // COMPASS_SYNC_BACKUP_DIR, default <database dir>/backups
	Interval time.Duration // COMPASS_SYNC_BACKUP_INTERVAL, default 24h; 0 = no backups
	Keep     int           // COMPASS_SYNC_BACKUP_KEEP, default 7
}

// backupWriter is what a backup needs from the store (datastore.SQLite).
type backupWriter interface {
	BackupTo(ctx context.Context, path string) error
}

// listBackups returns the backup files in dir, oldest first.
func listBackups(dir string) ([]string, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil, err
	}
	var names []string
	for _, e := range entries {
		n := e.Name()
		if e.Type().IsRegular() && strings.HasPrefix(n, backupPrefix) && strings.HasSuffix(n, backupSuffix) {
			names = append(names, n)
		}
	}
	slices.Sort(names)
	return names, nil
}

// backupTime is when a backup file was made, from its name.
func backupTime(name string) (time.Time, bool) {
	ts := strings.TrimSuffix(strings.TrimPrefix(name, backupPrefix), backupSuffix)
	t, err := time.Parse(backupTimeLayout, ts)
	return t, err == nil
}

// runBackup writes one backup to cfg.Dir (under a temporary name, renamed
// when complete) and deletes the oldest beyond cfg.Keep. Returns the new
// file's path.
func runBackup(ctx context.Context, db backupWriter, cfg BackupConfig, now time.Time) (string, error) {
	if err := os.MkdirAll(cfg.Dir, 0o700); err != nil {
		return "", fmt.Errorf("error creating %s: %w", cfg.Dir, err)
	}
	name := backupPrefix + now.UTC().Format(backupTimeLayout) + backupSuffix
	final := filepath.Join(cfg.Dir, name)
	tmp := filepath.Join(cfg.Dir, "."+name+".tmp")
	_ = os.Remove(tmp) // Left by a crash; VACUUM INTO needs a new file.
	if err := db.BackupTo(ctx, tmp); err != nil {
		_ = os.Remove(tmp)
		return "", err
	}
	if err := os.Rename(tmp, final); err != nil {
		_ = os.Remove(tmp)
		return "", fmt.Errorf("error renaming the backup: %w", err)
	}
	names, err := listBackups(cfg.Dir)
	if err != nil {
		return final, fmt.Errorf("error listing backups: %w", err)
	}
	keep := max(cfg.Keep, 1)
	for len(names) > keep {
		if err = os.Remove(filepath.Join(cfg.Dir, names[0])); err != nil {
			return final, fmt.Errorf("error removing an old backup: %w", err)
		}
		names = names[1:]
	}
	return final, nil
}

// nextBackupIn is how long to wait for the next backup: none if the newest
// one is older than the interval (or there is none).
func nextBackupIn(cfg BackupConfig, now time.Time) time.Duration {
	names, err := listBackups(cfg.Dir)
	if err != nil || len(names) == 0 {
		return 0
	}
	t, ok := backupTime(names[len(names)-1])
	if !ok {
		return 0
	}
	return max(t.Add(cfg.Interval).Sub(now), 0)
}

// backupLoop backs up every cfg.Interval until ctx ends. The first backup
// is due an interval after the newest one on disk, so restarts don't make
// extra ones.
func backupLoop(ctx context.Context, db backupWriter, cfg BackupConfig) {
	if cfg.Interval <= 0 {
		return
	}
	wait := nextBackupIn(cfg, time.Now())
	for {
		select {
		case <-ctx.Done():
			return
		case <-time.After(wait):
		}
		path, err := runBackup(ctx, db, cfg, time.Now())
		if err != nil {
			log.Error().Err(err).Msg("Backup failed")
			wait = min(cfg.Interval, time.Hour) // Try again sooner.
			continue
		}
		log.Info().Str("path", path).Msg("Backed up the database")
		wait = cfg.Interval
	}
}
