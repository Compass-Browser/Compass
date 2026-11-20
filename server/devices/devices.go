// Package devices removes a device from its sync chain
// (docs/design/sync.md, "Identity", "Removing a device"):
//
//	DELETE /{cache_guid}?client_id=<caller's cache GUID>   (sync token)
//
//	204                              removed (or removed already)
//	400 {"error":"bad_request"}      empty or oversized cache GUID or client_id
//	409 {"error":"this_device"}      the caller's own cache GUID
//	403 {"error":"chain_disabled"}   the chain was reset or expired
//
// The device's DEVICE_INFO entry is deleted as a commit of the deletion
// would (the other devices get the tombstone), its push registrations go,
// and its cache GUID is refused from then on (403 device_removed, see
// auth.RejectRemovedDevices). The cache GUID in the path is
// percent-encoded (Chromium's are base64 and may hold "/"). Every device
// holds the chain's seed, so this can't revoke one: the device keeps its
// data and can join again, as a new device (a new cache GUID).
package devices

import (
	"context"
	"encoding/json"
	"net/http"
	"net/url"
	"time"

	"github.com/go-chi/chi/v5"
	"github.com/rs/zerolog/log"

	"github.com/brave/go-sync/auth"
)

// MaxCacheGUID is the longest cache GUID accepted (as events'
// MaxClientID).
const MaxCacheGUID = 128

// Store is what the handler writes (datastore.SQLite).
type Store interface {
	IsSyncChainDisabled(ctx context.Context, clientID string) (bool, error)
	RemoveDevice(ctx context.Context, clientID, cacheGUID string, now time.Time) error
}

// Config wires the handler.
type Config struct {
	Store Store
	// Authorize checks a sync token, returning the chain ID
	// (auth.AuthorizeSync wrapped in auth.RejectRemovedDevices).
	Authorize func(*http.Request) (string, error)
	// DeleteDeviceInfo tombstones the device's DEVICE_INFO entity
	// (command.DeleteDeviceInfo).
	DeleteDeviceInfo func(ctx context.Context, chain, cacheGUID string) (bool, error)
	// Removed is told after a removal (the server notifies the chain's
	// other devices and ends the removed device's change stream). Optional.
	Removed func(chain, byCacheGUID, cacheGUID string)
}

// Handler serves removals.
type Handler struct{ cfg Config }

// New returns the handler.
func New(cfg Config) *Handler { return &Handler{cfg: cfg} }

// Routes returns the routes, to mount at /compass/devices.
func (h *Handler) Routes() chi.Router {
	r := chi.NewRouter()
	r.Delete("/", h.remove) // No cache GUID: 400.
	r.Delete("/{cache_guid}", h.remove)
	return r
}

func fail(w http.ResponseWriter, status int, msg string) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": msg})
}

// pathGUID returns the {cache_guid} parameter decoded. chi matches on the
// escaped path when there is one (a "%2F" in it), so it then comes encoded.
func pathGUID(r *http.Request) (string, bool) {
	v := chi.URLParam(r, "cache_guid")
	if r.URL.RawPath == "" {
		return v, true
	}
	d, err := url.PathUnescape(v)
	return d, err == nil
}

func (h *Handler) remove(w http.ResponseWriter, r *http.Request) {
	chain, err := h.cfg.Authorize(r)
	if chain == "" {
		if auth.WriteGateError(w, err) {
			return
		}
		fail(w, http.StatusUnauthorized, "invalid sync token")
		return
	}
	ctx := r.Context()
	guid, ok := pathGUID(r)
	self := r.URL.Query().Get("client_id")
	if !ok || guid == "" || len(guid) > MaxCacheGUID || self == "" || len(self) > MaxCacheGUID {
		fail(w, http.StatusBadRequest, "bad_request")
		return
	}
	if guid == self {
		fail(w, http.StatusConflict, "this_device")
		return
	}
	disabled, err := h.cfg.Store.IsSyncChainDisabled(ctx, chain)
	if err != nil {
		fail(w, http.StatusInternalServerError, "unable to complete request")
		return
	}
	if disabled {
		fail(w, http.StatusForbidden, "chain_disabled")
		return
	}
	// Refused first, so the device can't write its entry back in between.
	if err = h.cfg.Store.RemoveDevice(ctx, chain, guid, time.Now()); err != nil {
		log.Error().Err(err).Msg("Removing a device failed")
		fail(w, http.StatusInternalServerError, "unable to complete request")
		return
	}
	deleted, err := h.cfg.DeleteDeviceInfo(ctx, chain, guid)
	if err != nil {
		log.Error().Err(err).Msg("Deleting a removed device's entry failed")
		fail(w, http.StatusInternalServerError, "unable to complete request")
		return
	}
	log.Info().Str("chainID", chain).Str("device", guid).Bool("hadEntry", deleted).Msg("Device removed")
	if h.cfg.Removed != nil {
		h.cfg.Removed(chain, self, guid)
	}
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusNoContent)
}
