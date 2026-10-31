package datastore

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"time"
)

const (
	// Each period is roughly 3.5 days
	periodDurationSecs  int64 = HistoryExpirationIntervalSecs / 4
	CurrentCountVersion int   = 2
)

// ClientItemCounts is one row of the item_counts table: the number of
// non-deleted entities of a chain, with history items counted per period so
// they drop out of the count as they expire.
type ClientItemCounts struct {
	ClientID                string
	ID                      string
	ItemCount               int
	HistoryItemCountPeriod1 int
	HistoryItemCountPeriod2 int
	HistoryItemCountPeriod3 int
	HistoryItemCountPeriod4 int
	LastPeriodChangeTime    int64
	Version                 int
}

// ClientItemCountByClientID  implements sort.Interface for []ClientItemCount
// based on ClientID.
type ClientItemCountByClientID []ClientItemCounts

func (a ClientItemCountByClientID) Len() int      { return len(a) }
func (a ClientItemCountByClientID) Swap(i, j int) { a[i], a[j] = a[j], a[i] }
func (a ClientItemCountByClientID) Less(i, j int) bool {
	return a[i].ClientID < a[j].ClientID
}

func (counts *ClientItemCounts) SumHistoryCounts() int {
	return counts.HistoryItemCountPeriod1 +
		counts.HistoryItemCountPeriod2 +
		counts.HistoryItemCountPeriod3 +
		counts.HistoryItemCountPeriod4
}

func (s *SQLite) initRealCountsAndUpdateHistoryCounts(ctx context.Context, counts *ClientItemCounts) error {
	now := time.Now().Unix()
	if counts.Version < CurrentCountVersion {
		return s.migrateLegacyItemCounts(ctx, counts, now)
	}
	rotateHistoryCountPeriods(counts, now)
	return nil
}

func (s *SQLite) migrateLegacyItemCounts(ctx context.Context, counts *ClientItemCounts, now int64) error {
	if counts.ItemCount > 0 {
		if err := s.refreshItemCountsFromTable(ctx, counts); err != nil {
			return err
		}
	}
	counts.LastPeriodChangeTime = now
	counts.Version = CurrentCountVersion
	return nil
}

func (s *SQLite) refreshItemCountsFromTable(ctx context.Context, counts *ClientItemCounts) error {
	var historyCount, normalCount int
	err := s.r.QueryRowContext(ctx, `
		SELECT
			COALESCE(SUM(data_type IN (?, ?)), 0),
			COALESCE(SUM(data_type NOT IN (?, ?)), 0)
		FROM entities
		WHERE client_id = ? AND deleted = 0 AND data_type IS NOT NULL`,
		HistoryTypeID, HistoryDeleteDirectiveTypeID,
		HistoryTypeID, HistoryDeleteDirectiveTypeID,
		counts.ClientID).Scan(&historyCount, &normalCount)
	if err != nil {
		return fmt.Errorf("error counting items: %w", err)
	}
	counts.HistoryItemCountPeriod1 = 0
	counts.HistoryItemCountPeriod2 = 0
	counts.HistoryItemCountPeriod3 = 0
	counts.HistoryItemCountPeriod4 = historyCount
	counts.ItemCount = normalCount
	return nil
}

func rotateHistoryCountPeriods(counts *ClientItemCounts, now int64) {
	timeSinceLastChange := now - counts.LastPeriodChangeTime
	if timeSinceLastChange < periodDurationSecs {
		return
	}
	changeCount := int(timeSinceLastChange / periodDurationSecs)
	for range changeCount {
		// The records from "period 1"/the earliest period
		// will be purged from the count, since they expire (PurgeExpired).
		counts.HistoryItemCountPeriod1 = counts.HistoryItemCountPeriod2
		counts.HistoryItemCountPeriod2 = counts.HistoryItemCountPeriod3
		counts.HistoryItemCountPeriod3 = counts.HistoryItemCountPeriod4
		counts.HistoryItemCountPeriod4 = 0
	}
	counts.LastPeriodChangeTime += periodDurationSecs * int64(changeCount)
}

const itemCountColumns = `client_id, item_count, history_period1, history_period2,
	history_period3, history_period4, last_period_change_time, version`

func scanItemCounts(row interface{ Scan(...any) error }) (ClientItemCounts, error) {
	var c ClientItemCounts
	err := row.Scan(&c.ClientID, &c.ItemCount, &c.HistoryItemCountPeriod1,
		&c.HistoryItemCountPeriod2, &c.HistoryItemCountPeriod3,
		&c.HistoryItemCountPeriod4, &c.LastPeriodChangeTime, &c.Version)
	c.ID = c.ClientID
	return c, err
}

// GetClientItemCount returns the count of non-deleted sync items stored for
// a given client.
func (s *SQLite) GetClientItemCount(ctx context.Context, clientID string) (*ClientItemCounts, error) {
	counts, err := scanItemCounts(s.r.QueryRowContext(ctx,
		`SELECT `+itemCountColumns+` FROM item_counts WHERE client_id = ?`, clientID))
	if errors.Is(err, sql.ErrNoRows) {
		counts = ClientItemCounts{ClientID: clientID, ID: clientID}
	} else if err != nil {
		return nil, fmt.Errorf("error getting an item-count item: %w", err)
	}

	if err = s.initRealCountsAndUpdateHistoryCounts(ctx, &counts); err != nil {
		return nil, err
	}

	return &counts, nil
}

// UpdateClientItemCount updates the count of non-deleted sync items for a
// given client.
func (s *SQLite) UpdateClientItemCount(
	ctx context.Context,
	counts *ClientItemCounts,
	newNormalItemCount int,
	newHistoryItemCount int,
) error {
	counts.HistoryItemCountPeriod4 += newHistoryItemCount
	counts.ItemCount += newNormalItemCount

	_, err := s.w.ExecContext(ctx, `
		INSERT OR REPLACE INTO item_counts (`+itemCountColumns+`)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?)`,
		counts.ClientID, counts.ItemCount, counts.HistoryItemCountPeriod1,
		counts.HistoryItemCountPeriod2, counts.HistoryItemCountPeriod3,
		counts.HistoryItemCountPeriod4, counts.LastPeriodChangeTime, counts.Version)
	if err != nil {
		return fmt.Errorf("error updating item-count item: %w", err)
	}
	return nil
}
