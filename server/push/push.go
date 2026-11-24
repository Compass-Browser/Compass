package push

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"slices"
	"strings"
	"sync"
	"time"

	"github.com/rs/zerolog/log"

	"github.com/brave/go-sync/datastore"

	"github.com/brave/go-sync/auth"
)

// SendTabToSelfType is SEND_TAB_TO_SELF's EntitySpecifics field number.
const SendTabToSelfType = 601980

// The types whose changes wake a chain's iPhones in the background
// (send-tab.md, "Background sync"): what the phone shows of another
// device's work. Not HISTORY, SESSIONS or DEVICE_INFO, which change all the
// time while a Mac is in use and can wait for the next sync; not
// SEND_TAB_TO_SELF, which has its own alert.
var BackgroundTypes = []int{
	970000, // COMPASS_SPACES: spaces, folders, pinned and favorite entries.
	45873,  // PASSWORDS.
	48119,  // EXTENSIONS.
}

// DefaultBackgroundInterval is the least time between two background
// pushes to one device: iOS gives an app a few an hour, and drops the rest.
const DefaultBackgroundInterval = 15 * time.Minute

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
	// FCM, for Compass for Android (fcm.go).
	FCM FCMConfig
	// Streaming, if set, says whether a device has a change stream open
	// (events.Hub.Connected): Compass is open there and syncs from the
	// stream, so it isn't sent a background push.
	Streaming func(chain, cacheGUID string) bool
	// BackgroundInterval is the least time between two background pushes to
	// a device; DefaultBackgroundInterval if zero.
	BackgroundInterval time.Duration
	// Relay, if set, is the Compass push relay's URL (relay.go), used when
	// neither APNs nor FCM is configured; RelayClient, if set, replaces
	// the default HTTP client (tests).
	Relay       string
	RelayClient *http.Client
	// PublicHost is the server's name the relay binds handles to; empty:
	// the Host each registration came to.
	PublicHost string
}

// The alert a new sent tab wakes a phone with; its notification service
// extension rewrites it ("Tab from MacBook", the page's title and host).
var sendTabPayload = []byte(`{"aps":{"alert":{"title":"New tab","body":"Open Compass to see it"},` +
	`"mutable-content":1,"sound":"default","thread-id":"sent-tab"},"compass":{"type":"send-tab"}}`)

// The silent push that has an iPhone sync in the background.
var syncPayload = []byte(`{"aps":{"content-available":1},"compass":{"type":"sync"}}`)

// maxConcurrent bounds the pushes in flight.
const maxConcurrent = 16

// Service serves registrations and sends the pushes.
type Service struct {
	cfg    Config
	sender *Sender      // Nil without a key: registrations are kept, nothing sent.
	fcm    *FCMSender   // Nil without a service account, likewise.
	relay  *RelayClient // Set only without either (relay.go).
	slots  chan struct{}
	now    func() time.Time
	// sent is told about each push (tests).
	sent func(datastore.PushToken, Result, error)

	// Background pushes by chain and cache GUID: when the last went out,
	// and the one waiting for the interval to pass.
	bgMu sync.Mutex
	bg   map[bgKey]*bgState
}

type bgKey struct{ chain, cacheGUID string }

type bgState struct {
	last    time.Time
	pending *time.Timer
}

// New makes the service; it fails if the APNs key can't be loaded.
func New(cfg Config) (*Service, error) {
	if len(cfg.Topics) == 0 {
		cfg.Topics = []string{DefaultTopic}
	}
	if cfg.BackgroundInterval <= 0 {
		cfg.BackgroundInterval = DefaultBackgroundInterval
	}
	s := &Service{cfg: cfg, slots: make(chan struct{}, maxConcurrent), now: time.Now, bg: map[bgKey]*bgState{}}
	if cfg.APNs.Enabled() {
		sender, err := NewSender(cfg.APNs)
		if err != nil {
			return nil, err
		}
		s.sender = sender
	}
	if cfg.FCM.Enabled() {
		fcm, err := NewFCMSender(cfg.FCM)
		if err != nil {
			return nil, err
		}
		s.fcm = fcm
	}
	if s.sender == nil && s.fcm == nil && cfg.Relay != "" {
		s.relay = NewRelayClient(cfg.Relay, cfg.RelayClient)
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
		if auth.WriteGateError(w, err) {
			return
		}
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
		// "apns" (the default, as older iOS clients send none) or "fcm".
		Platform string `json:"platform"`
	}
	raw, err := io.ReadAll(io.LimitReader(r.Body, 8192))
	if err != nil || json.Unmarshal(raw, &body) != nil {
		fail(w, http.StatusBadRequest, "JSON body required")
		return
	}
	if body.Platform == "fcm" {
		switch {
		case !validFCMToken(body.Token):
			fail(w, http.StatusBadRequest, "token: FCM registration token required")
			return
		case !slices.Contains(s.cfg.Topics, body.Topic):
			fail(w, http.StatusBadRequest, "topic: not an app this server pushes to")
			return
		}
		// FCM registrations are marked by their environment.
		body.Environment = FCMEnvironment
	} else {
		body.Token = strings.ToLower(body.Token)
	}
	switch {
	case body.Platform != "" && body.Platform != "apns" && body.Platform != "fcm":
		fail(w, http.StatusBadRequest, `platform: "apns" or "fcm"`)
		return
	case body.Platform == "fcm":
		// Checked above.
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
	if s.relay != nil {
		// Keep the relay's handle, never the token.
		host := s.cfg.PublicHost
		if host == "" {
			host = r.Host
		}
		host = strings.ToLower(host)
		platform, kind := "apns", RelayAPNs
		if body.Platform == "fcm" {
			platform, kind = "fcm", RelayFCM
		}
		handle, err := s.relay.Seal(ctx, host, platform, body.Token, body.Environment, body.Topic)
		if errors.Is(err, ErrRelayRefused) {
			fail(w, http.StatusBadRequest, "the push relay refused this registration")
			return
		}
		if err != nil {
			log.Warn().Err(err).Msg("Push: sealing at the relay failed")
			fail(w, http.StatusServiceUnavailable, "the push relay can't be reached; register again later")
			return
		}
		tok = datastore.PushToken{CacheGUID: guid, Token: handle, Environment: kind, Topic: host}
	}
	if err = s.cfg.Store.SetPushToken(ctx, chain, tok, s.now()); err != nil {
		log.Error().Err(err).Msg("Push: register failed")
		fail(w, http.StatusInternalServerError, "unable to complete request")
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

// OnCommit is told what a commit by |cacheGUID| stored in |chain|: |types|
// are the types it changed, |created| those it added entities of. A new
// SEND_TAB_TO_SELF entity wakes the chain's other registered devices with
// an alert; a change of one of BackgroundTypes has its other iPhones sync
// in the background (at most once per BackgroundInterval each). It returns
// at once; the pushes go out in the background.
func (s *Service) OnCommit(chain, cacheGUID string, types, created []int) {
	sendTab := slices.Contains(created, SendTabToSelfType)
	background := slices.ContainsFunc(types, func(t int) bool { return slices.Contains(BackgroundTypes, t) })
	if !sendTab && !background {
		return
	}
	if s.sender == nil && s.fcm == nil && s.relay == nil {
		if sendTab {
			log.Debug().Msg("Push: a tab was sent, but no APNs key or FCM account is configured")
		}
		return
	}
	if sendTab {
		go s.wake(chain, cacheGUID)
	}
	if background && (s.sender != nil || s.relay != nil) {
		go s.wakeBackground(chain, cacheGUID)
	}
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
		if !s.send(ctx, chain, t, sendTabPayload, false) {
			return
		}
	}
}

// wakeBackground has |chain|'s iPhones but |except| sync: a silent push to
// each now, or once its interval since the last one has passed.
func (s *Service) wakeBackground(chain, except string) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	tokens, err := s.cfg.Store.PushTokens(ctx, chain)
	if err != nil {
		log.Error().Err(err).Msg("Push: reading tokens failed")
		return
	}
	for _, t := range tokens {
		if t.CacheGUID == except || t.Environment == FCMEnvironment || t.Environment == RelayFCM {
			continue
		}
		s.scheduleBackground(chain, t.CacheGUID)
	}
}

func (s *Service) scheduleBackground(chain, cacheGUID string) {
	// Compass is open there: the stream has it sync now.
	if s.cfg.Streaming != nil && s.cfg.Streaming(chain, cacheGUID) {
		return
	}
	key := bgKey{chain, cacheGUID}
	s.bgMu.Lock()
	defer s.bgMu.Unlock()
	now := s.now()
	s.pruneBackground(now)
	st := s.bg[key]
	if st == nil {
		st = &bgState{}
		s.bg[key] = st
	}
	if st.pending != nil {
		// Already waiting: that push covers this change too.
		return
	}
	wait := st.last.Add(s.cfg.BackgroundInterval).Sub(now)
	if st.last.IsZero() || wait <= 0 {
		st.last = now
		go s.sendBackground(chain, cacheGUID)
		return
	}
	st.pending = time.AfterFunc(wait, func() {
		s.bgMu.Lock()
		st.pending = nil
		st.last = s.now()
		s.bgMu.Unlock()
		s.sendBackground(chain, cacheGUID)
	})
}

// pruneBackground forgets devices whose interval has passed with nothing
// waiting (under bgMu).
func (s *Service) pruneBackground(now time.Time) {
	if len(s.bg) < 256 {
		return
	}
	for k, st := range s.bg {
		if st.pending == nil && now.Sub(st.last) >= s.cfg.BackgroundInterval {
			delete(s.bg, k)
		}
	}
}

// sendBackground sends the silent push to |cacheGUID|'s registration, as it
// is now, unless Compass is open there.
func (s *Service) sendBackground(chain, cacheGUID string) {
	if s.cfg.Streaming != nil && s.cfg.Streaming(chain, cacheGUID) {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	tokens, err := s.cfg.Store.PushTokens(ctx, chain)
	if err != nil {
		log.Error().Err(err).Msg("Push: reading tokens failed")
		return
	}
	for _, t := range tokens {
		if t.CacheGUID == cacheGUID && t.Environment != FCMEnvironment && t.Environment != RelayFCM {
			s.send(ctx, chain, t, syncPayload, true)
		}
	}
}

// send pushes |payload| to |t| (an FCM registration gets its wake-up
// instead), dropping a token the service says is gone. It returns false
// once |ctx| is done.
func (s *Service) send(ctx context.Context, chain string, t datastore.PushToken, payload []byte, background bool) bool {
	select {
	case s.slots <- struct{}{}:
	case <-ctx.Done():
		return false
	}
	if isRelay(t.Environment) {
		s.sendThroughRelay(ctx, chain, t, background)
		<-s.slots
		return true
	}
	fcm := t.Environment == FCMEnvironment
	var res Result
	var err error
	switch {
	case fcm && s.fcm != nil:
		res, err = s.fcm.Send(ctx, t.Token, "send-tab")
	case !fcm && s.sender != nil && background:
		res, err = s.sender.SendBackground(ctx, t.Environment, t.Topic, t.Token, payload)
	case !fcm && s.sender != nil:
		res, err = s.sender.Send(ctx, t.Environment, t.Topic, t.Token, payload)
	default:
		// This platform has no credentials here.
		<-s.slots
		return true
	}
	<-s.slots
	if s.sent != nil {
		s.sent(t, res, err)
	}
	switch {
	case err != nil:
		log.Warn().Err(err).Msg("Push: send failed")
	case fcm && FCMInvalid(res), !fcm && res.Invalid():
		log.Info().Int("status", res.Status).Str("reason", res.Reason).Msg("Push: dropping an invalid token")
		if err = s.cfg.Store.DropPushToken(ctx, chain, t.Token); err != nil {
			log.Error().Err(err).Msg("Push: dropping a token failed")
		}
	case res.Status != http.StatusOK:
		log.Warn().Int("status", res.Status).Str("reason", res.Reason).Bool("fcm", fcm).
			Bool("background", background).Msg("Push: the push was refused")
	}
	return true
}

// sendThroughRelay asks the relay to push to the handle |t| holds (relay.go),
// forgetting it if the relay says it's no good.
func (s *Service) sendThroughRelay(ctx context.Context, chain string, t datastore.PushToken, background bool) {
	if s.relay == nil {
		// Made through a relay this server no longer uses.
		return
	}
	kind := "send-tab"
	if background {
		kind = "sync"
	}
	drop, err := s.relay.Push(ctx, t.Topic, t.Token, kind)
	switch {
	case err != nil:
		log.Warn().Err(err).Msg("Push: the relay failed")
	case drop:
		log.Info().Msg("Push: dropping a handle the relay refused")
		if err = s.cfg.Store.DropPushToken(ctx, chain, t.Token); err != nil {
			log.Error().Err(err).Msg("Push: dropping a handle failed")
		}
	}
	if s.sent != nil {
		status := http.StatusOK
		if err != nil || drop {
			status = http.StatusGone
		}
		s.sent(t, Result{Status: status}, err)
	}
}
