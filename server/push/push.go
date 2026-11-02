package push

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"slices"
	"strings"
	"time"

	"github.com/rs/zerolog/log"

	"github.com/brave/go-sync/datastore"
)

// SendTabToSelfType is SEND_TAB_TO_SELF's EntitySpecifics field number.
const SendTabToSelfType = 601980

// DefaultTopic is Compass for iOS's bundle ID.
const DefaultTopic = "it.cipi1965.compass"

// Store is what the package reads and writes (datastore.SQLite).
type Store interface {
	SetPushToken(ctx context.Context, clientID string, tok datastore.PushToken, now time.Time) error
	DeletePushToken(ctx context.Context, clientID, cacheGUID string) error
	DropPushToken(ctx context.Context, clientID, token string) error
	PushTokens(ctx context.Context, clientID string) ([]datastore.PushToken, error)
	IsSyncChainDisabled(ctx context.Context, clientID string) (bool, error)
}

// Config wires the package.
type Config struct {
	Store Store
	// Authorize checks a sync token, returning the chain ID (auth.Authorize).
	Authorize func(*http.Request) (string, error)
	// Topics are the bundle IDs devices may register for.
	Topics []string
	APNs   APNsConfig
}

// The alert a new sent tab wakes a phone with; its notification service
// extension rewrites it ("Tab from MacBook", the page's title and host).
var sendTabPayload = []byte(`{"aps":{"alert":{"title":"New tab","body":"Open Compass to see it"},` +
	`"mutable-content":1,"sound":"default","thread-id":"sent-tab"},"compass":{"type":"send-tab"}}`)

// maxConcurrent bounds the pushes in flight.
const maxConcurrent = 16

// Service serves registrations and sends the pushes.
type Service struct {
	cfg    Config
	sender *Sender // Nil without a key: registrations are kept, nothing sent.
	slots  chan struct{}
	now    func() time.Time
	// sent is told about each push (tests).
	sent func(datastore.PushToken, Result, error)
}

// New makes the service; it fails if the APNs key can't be loaded.
func New(cfg Config) (*Service, error) {
	if len(cfg.Topics) == 0 {
		cfg.Topics = []string{DefaultTopic}
	}
	s := &Service{cfg: cfg, slots: make(chan struct{}, maxConcurrent), now: time.Now}
	if cfg.APNs.Enabled() {
		sender, err := NewSender(cfg.APNs)
		if err != nil {
			return nil, err
		}
		s.sender = sender
	}
	return s, nil
}

// SetSentHookForTesting sets a function told about each push.
func (s *Service) SetSentHookForTesting(f func(datastore.PushToken, Result, error)) { s.sent = f }

func fail(w http.ResponseWriter, status int, msg string) {
	http.Error(w, msg, status)
}

const maxCacheGUID = 64

func validToken(t string) bool {
	if len(t) < 32 || len(t) > 200 || len(t)%2 != 0 {
		return false
	}
	_, err := hex.DecodeString(t)
	return err == nil
}

func (s *Service) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost && r.Method != http.MethodDelete {
		w.Header().Set("Allow", "POST, DELETE")
		fail(w, http.StatusMethodNotAllowed, "POST or DELETE")
		return
	}
	chain, err := s.cfg.Authorize(r)
	if err != nil || chain == "" {
		fail(w, http.StatusUnauthorized, "invalid sync token")
		return
	}
	ctx := r.Context()
	disabled, err := s.cfg.Store.IsSyncChainDisabled(ctx, chain)
	if err != nil {
		fail(w, http.StatusInternalServerError, "unable to complete request")
		return
	}
	if disabled {
		fail(w, http.StatusForbidden, "sync chain disabled")
		return
	}
	guid := r.URL.Query().Get("client_id")
	if guid == "" || len(guid) > maxCacheGUID {
		fail(w, http.StatusBadRequest, "client_id required (the device's cache GUID)")
		return
	}
	if r.Method == http.MethodDelete {
		if err = s.cfg.Store.DeletePushToken(ctx, chain, guid); err != nil {
			log.Error().Err(err).Msg("Push: unregister failed")
			fail(w, http.StatusInternalServerError, "unable to complete request")
			return
		}
		w.WriteHeader(http.StatusNoContent)
		return
	}
	var body struct {
		Token       string `json:"token"`
		Environment string `json:"environment"`
		Topic       string `json:"topic"`
	}
	raw, err := io.ReadAll(io.LimitReader(r.Body, 4096))
	if err != nil || json.Unmarshal(raw, &body) != nil {
		fail(w, http.StatusBadRequest, "JSON body required")
		return
	}
	body.Token = strings.ToLower(body.Token)
	switch {
	case !validToken(body.Token):
		fail(w, http.StatusBadRequest, "token: hex device token required")
		return
	case body.Environment != "sandbox" && body.Environment != "production":
		fail(w, http.StatusBadRequest, `environment: "sandbox" or "production"`)
		return
	case !slices.Contains(s.cfg.Topics, body.Topic):
		fail(w, http.StatusBadRequest, "topic: not an app this server pushes to")
		return
	}
	tok := datastore.PushToken{CacheGUID: guid, Token: body.Token,
		Environment: body.Environment, Topic: body.Topic}
	if err = s.cfg.Store.SetPushToken(ctx, chain, tok, s.now()); err != nil {
		log.Error().Err(err).Msg("Push: register failed")
		fail(w, http.StatusInternalServerError, "unable to complete request")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// OnCommit is told what a commit by |cacheGUID| stored in |chain|: |created|
// are the types of the entities it added. A new SEND_TAB_TO_SELF entity
// wakes the chain's other registered devices. It returns at once; the
// pushes go out in the background.
func (s *Service) OnCommit(chain, cacheGUID string, created []int) {
	if !slices.Contains(created, SendTabToSelfType) {
		return
	}
	if s.sender == nil {
		log.Debug().Msg("Push: a tab was sent, but no APNs key is configured")
		return
	}
	go s.wake(chain, cacheGUID)
}

func (s *Service) wake(chain, except string) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	tokens, err := s.cfg.Store.PushTokens(ctx, chain)
	if err != nil {
		log.Error().Err(err).Msg("Push: reading tokens failed")
		return
	}
	for _, t := range tokens {
		if t.CacheGUID == except {
			continue
		}
		select {
		case s.slots <- struct{}{}:
		case <-ctx.Done():
			return
		}
		res, err := s.sender.Send(ctx, t.Environment, t.Topic, t.Token, sendTabPayload)
		<-s.slots
		if s.sent != nil {
			s.sent(t, res, err)
		}
		switch {
		case err != nil:
			log.Warn().Err(err).Msg("Push: send failed")
		case res.Invalid():
			log.Info().Int("status", res.Status).Str("reason", res.Reason).Msg("Push: dropping an invalid token")
			if err = s.cfg.Store.DropPushToken(ctx, chain, t.Token); err != nil {
				log.Error().Err(err).Msg("Push: dropping a token failed")
			}
		case res.Status != http.StatusOK:
			log.Warn().Int("status", res.Status).Str("reason", res.Reason).Msg("Push: APNs refused the push")
		}
	}
}
