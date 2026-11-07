// Package quota tells a chain's devices how close the chain is to its
// limits, and which limit refused its latest commits (docs/design/sync.md,
// "Server"). Chromium's sync engine treats an OVER_QUOTA entry as a
// transient error and retries quietly, and a dropped visit (history quota)
// answers SUCCESS, so Compass asks here to show it.
//
//	GET /   (sync token)   200 application/json
//
//	{"entities":1200,"max_entities":100000,
//	 "bytes":5242880,"max_bytes":209715200,
//	 "history":30000,"max_history":30000,"history_days":14,
//	 "full":["history"],"full_since":1793491200}
//
// "full" lists the limits in the way now: "entities" (live entities, history
// included), "bytes" (stored bytes) and "history" (visits within the
// retention). "full_since" is when a commit was last refused (Unix seconds,
// 0 if never). Counts are the server's own bookkeeping, nothing a device
// sent in the clear.
package quota

import (
	"context"
	"encoding/json"
	"net/http"
	"time"

	"github.com/rs/zerolog/log"

	"github.com/brave/go-sync/datastore"
)

// Store is what the handler reads (datastore.SQLite).
type Store interface {
	GetClientItemCount(ctx context.Context, clientID string) (*datastore.ClientItemCounts, error)
	ChainBytes(ctx context.Context, clientID string) (int64, error)
	QuotaState(ctx context.Context, clientID string) (datastore.QuotaFull, time.Time, error)
	IsSyncChainDisabled(ctx context.Context, clientID string) (bool, error)
}

// Limits are a chain's limits; 0 means none.
type Limits struct {
	MaxEntities int
	MaxBytes    int64
	MaxHistory  int
	HistoryDays int
}

// Config wires the handler.
type Config struct {
	Store Store
	// Authorize checks a sync token, returning the chain ID (auth.Authorize).
	Authorize func(*http.Request) (string, error)
	// Limits returns the limits in force.
	Limits func() Limits
}

// Status is the JSON answer.
type Status struct {
	Entities    int      `json:"entities"`
	MaxEntities int      `json:"max_entities"`
	Bytes       int64    `json:"bytes"`
	MaxBytes    int64    `json:"max_bytes"`
	History     int      `json:"history"`
	MaxHistory  int      `json:"max_history"`
	HistoryDays int      `json:"history_days"`
	Full        []string `json:"full"`
	FullSince   int64    `json:"full_since"`
}

// Handler serves the status of the caller's chain.
type Handler struct{ cfg Config }

// New returns the handler.
func New(cfg Config) *Handler { return &Handler{cfg: cfg} }

// bytesFullMargin: a chain this close to its byte limit counts as full even
// without a refusal on record (a large entry would not fit).
const bytesFullMargin = 64 << 10

// Compute returns the status of |chain|.
func (h *Handler) Compute(ctx context.Context, chain string) (Status, error) {
	limits := h.cfg.Limits()
	counts, err := h.cfg.Store.GetClientItemCount(ctx, chain)
	if err != nil {
		return Status{}, err
	}
	bytes, err := h.cfg.Store.ChainBytes(ctx, chain)
	if err != nil {
		return Status{}, err
	}
	refused, since, err := h.cfg.Store.QuotaState(ctx, chain)
	if err != nil {
		return Status{}, err
	}
	history := counts.SumHistoryCounts()
	st := Status{
		Entities:    counts.ItemCount + history,
		MaxEntities: limits.MaxEntities,
		Bytes:       bytes,
		MaxBytes:    limits.MaxBytes,
		History:     history,
		MaxHistory:  limits.MaxHistory,
		HistoryDays: limits.HistoryDays,
		Full:        []string{},
	}
	if !since.IsZero() {
		st.FullSince = since.Unix()
	}
	if refused&datastore.QuotaEntities != 0 ||
		(limits.MaxEntities > 0 && st.Entities >= limits.MaxEntities) {
		st.Full = append(st.Full, "entities")
	}
	if refused&datastore.QuotaBytes != 0 ||
		(limits.MaxBytes > 0 && bytes+bytesFullMargin > limits.MaxBytes) {
		st.Full = append(st.Full, "bytes")
	}
	if refused&datastore.QuotaHistory != 0 ||
		(limits.MaxHistory > 0 && history >= limits.MaxHistory) {
		st.Full = append(st.Full, "history")
	}
	return st, nil
}

func (h *Handler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	chain, err := h.cfg.Authorize(r)
	if chain == "" {
		if err != nil {
			log.Debug().Err(err).Msg("Quota status: authorization failed")
		}
		http.Error(w, http.StatusText(http.StatusUnauthorized), http.StatusUnauthorized)
		return
	}
	disabled, err := h.cfg.Store.IsSyncChainDisabled(r.Context(), chain)
	if err != nil {
		http.Error(w, "unable to complete request", http.StatusInternalServerError)
		return
	}
	if disabled {
		http.Error(w, "chain disabled", http.StatusForbidden)
		return
	}
	st, err := h.Compute(r.Context(), chain)
	if err != nil {
		log.Error().Err(err).Msg("Quota status failed")
		http.Error(w, "unable to complete request", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	_ = json.NewEncoder(w).Encode(st)
}
