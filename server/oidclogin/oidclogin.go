// Package oidclogin is the sync server's optional company sign-in
// (docs/design/sync.md, "Company sign-in (OIDC)"). The server is the OIDC
// relying party (authorization code flow with PKCE); a client creates a
// login request with its chain token, opens the returned URL in a browser
// and long-polls for the result. After the identity provider, the server
// shows the device name and a short code the client also shows, and binds
// the chain to the user when the person allows it. Sign-in gates access to
// the server and never touches the keys.
//
//	GET    /v2/compass/info                    no auth       200 {"login": null | {"type":"oidc","name"}}
//	GET    /v2/compass/account                 chain token   200 {"login_required","user","expires"}
//	DELETE /v2/compass/account                 chain token   204 (unbinds)
//	POST   /v2/compass/account/link            bound chain + X-Compass-Linked-Token   204
//	POST   /v2/compass/login                   chain token   200 {"id","url","user_code","expires"}
//	GET    /v2/compass/login/{id}?wait=0       same chain    200 {"status","user"} (long poll ≤ 30 s)
//	GET    /v2/compass/login/{id}/start        browser       302 to the identity provider
//	GET    /v2/compass/login/callback          browser       consent page
//	POST   /v2/compass/login/{id}/approve|deny browser, CSRF done page, or 302 compass://sync-login-done
//
// Pending logins live in memory (a restart drops them; the client starts
// again). Users and bindings are in the datastore (Store).
package oidclogin

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/coreos/go-oidc/v3/oidc"
	"github.com/go-chi/chi/v5"
	"github.com/rs/zerolog/log"
	"golang.org/x/oauth2"

	"github.com/brave/go-sync/auth"
	"github.com/brave/go-sync/datastore"
)

// LinkedTokenHeader carries the other chain's sync token on
// POST /v2/compass/account/link.
const LinkedTokenHeader = "X-Compass-Linked-Token"

// Store is what sign-in needs from the datastore (datastore.SQLite).
type Store interface {
	SaveOIDCLogin(ctx context.Context, login datastore.OIDCLogin, clientID string,
		sessionExpires, now time.Time) (datastore.OIDCUser, error)
	OIDCUserRevoked(ctx context.Context, issuer, subject string) (bool, error)
	ChainBinding(ctx context.Context, clientID string) (*datastore.ChainBinding, error)
	LinkChain(ctx context.Context, from, to string, now time.Time) (bool, error)
	UnbindChain(ctx context.Context, clientID string) error
	OIDCUsersToRevalidate(ctx context.Context, issuer string, before time.Time) ([]datastore.OIDCUser, error)
	UpdateOIDCUser(ctx context.Context, id int64, refreshToken, email, name string, now time.Time) error
	EndOIDCUserSessions(ctx context.Context, id int64, revoke bool) ([]string, error)
}

// Service is the sign-in service. With sign-in off it only answers /info,
// /account (never login_required) and 404 login_disabled.
type Service struct {
	cfg Config

	ready    chan struct{} // Closed once the provider is discovered.
	provider *oidc.Provider
	verifier *oidc.IDTokenVerifier
	oauth    *oauth2.Config

	mu        sync.Mutex
	logins    map[string]*pending
	byState   map[string]string // OAuth state → login ID.
	byChain   map[string][]time.Time
	byIP      map[string][]time.Time
	lastSweep time.Time

	cacheMu sync.Mutex
	cache   map[string]cached
}

type cached struct {
	binding *datastore.ChainBinding
	until   time.Time
}

// New returns the service for |cfg|. With sign-in on it discovers the
// identity provider, retrying in the background (until |ctx| ends) if it
// can't be reached: logins answer 503 until then; bound chains sync.
func New(ctx context.Context, cfg Config) (*Service, error) {
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	if cfg.HTTPClient == nil {
		cfg.HTTPClient = http.DefaultClient
	}
	if cfg.ClientIP == nil {
		cfg.ClientIP = func(r *http.Request) string { return r.RemoteAddr }
	}
	s := &Service{
		cfg:     cfg,
		ready:   make(chan struct{}),
		logins:  map[string]*pending{},
		byState: map[string]string{},
		byChain: map[string][]time.Time{},
		byIP:    map[string][]time.Time{},
		cache:   map[string]cached{},
	}
	if !cfg.Enabled() {
		return s, nil
	}
	if cfg.Store == nil {
		return nil, errors.New("oidclogin: no store")
	}
	if cfg.ClientID == "" || cfg.PublicHost == "" {
		return nil, errors.New("oidclogin: sign-in needs a client ID and the public host")
	}
	if err := s.discover(ctx); err != nil {
		log.Error().Err(err).Str("issuer", cfg.Issuer).
			Msg("Sign-in: can't reach the identity provider; retrying in the background")
		go s.discoverLoop(ctx)
	}
	return s, nil
}

// Enabled says whether sign-in is on.
func (s *Service) Enabled() bool { return s.cfg.Enabled() }

// Ready says whether the identity provider has been discovered.
func (s *Service) Ready() bool {
	select {
	case <-s.ready:
		return true
	default:
		return false
	}
}

func (s *Service) clientCtx(ctx context.Context) context.Context {
	ctx = oidc.ClientContext(ctx, s.cfg.HTTPClient)
	return context.WithValue(ctx, oauth2.HTTPClient, s.cfg.HTTPClient)
}

func (s *Service) discover(ctx context.Context) error {
	dctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	// The key set the provider makes fetches with the client of the context
	// given here, and later with the context of each Verify.
	p, err := oidc.NewProvider(s.clientCtx(dctx), s.cfg.Issuer)
	if err != nil {
		return fmt.Errorf("discovering %s: %w", s.cfg.Issuer, err)
	}
	s.provider = p
	s.verifier = p.Verifier(&oidc.Config{ClientID: s.cfg.ClientID})
	s.oauth = &oauth2.Config{
		ClientID:     s.cfg.ClientID,
		ClientSecret: s.cfg.ClientSecret,
		Endpoint:     p.Endpoint(),
		RedirectURL:  s.RedirectURL(),
		Scopes:       s.cfg.Scopes,
	}
	close(s.ready)
	log.Info().Str("issuer", s.cfg.Issuer).Msg("Sign-in: identity provider discovered")
	return nil
}

func (s *Service) discoverLoop(ctx context.Context) {
	wait := 5 * time.Second
	for {
		select {
		case <-ctx.Done():
			return
		case <-time.After(wait):
		}
		err := s.discover(ctx)
		if err == nil {
			return
		}
		log.Warn().Err(err).Msg("Sign-in: identity provider still unreachable")
		wait = min(wait*2, 5*time.Minute)
	}
}

// RedirectURL is the redirect URI to register with the identity provider.
func (s *Service) RedirectURL() string {
	return BaseURL(s.cfg.PublicHost) + "/v2/compass/login/callback"
}

// ProviderName is the name users see: COMPASS_SYNC_OIDC_NAME, else the
// issuer's host.
func (s *Service) ProviderName() string {
	if s.cfg.Name != "" {
		return s.cfg.Name
	}
	if u, err := url.Parse(s.cfg.Issuer); err == nil && u.Host != "" {
		return u.Host
	}
	return s.cfg.Issuer
}

// Register adds the routes to |r| (the /v2 router).
func (s *Service) Register(r chi.Router) {
	r.Get("/compass/info", s.info)
	r.Get("/compass/account", s.account)
	r.Delete("/compass/account", s.signOut)
	r.Post("/compass/account/link", s.link)
	r.Post("/compass/login", s.create)
	r.Get("/compass/login/callback", s.callback)
	r.Get("/compass/login/{id}", s.poll)
	r.Get("/compass/login/{id}/start", s.start)
	r.Post("/compass/login/{id}/approve", s.approve)
	r.Post("/compass/login/{id}/deny", s.deny)
}

// Authorize is the token check for every sync endpoint: auth.Authorize
// with sign-in off, else auth.AuthorizeSync with Gate.
func (s *Service) Authorize() func(*http.Request) (string, error) {
	if !s.Enabled() {
		return auth.Authorize
	}
	return auth.AuthorizeSync(s.Gate)
}

// Gate says whether chain |chain| (lower-case) may sync: nil if it's bound
// to a user that may, auth.ErrLoginRequired if not.
func (s *Service) Gate(ctx context.Context, chain string) error {
	b, err := s.binding(ctx, chain)
	if err != nil {
		return err
	}
	if !b.Valid(s.cfg.Issuer, s.cfg.Now()) {
		return auth.ErrLoginRequired
	}
	return nil
}

// binding is ChainBinding through a short cache (every sync request asks).
func (s *Service) binding(ctx context.Context, chain string) (*datastore.ChainBinding, error) {
	chain = strings.ToLower(chain)
	now := s.cfg.Now()
	if s.cfg.GateCacheTTL > 0 {
		s.cacheMu.Lock()
		c, ok := s.cache[chain]
		s.cacheMu.Unlock()
		if ok && now.Before(c.until) {
			return c.binding, nil
		}
	}
	b, err := s.cfg.Store.ChainBinding(ctx, chain)
	if err != nil {
		return nil, err
	}
	if s.cfg.GateCacheTTL > 0 {
		s.cacheMu.Lock()
		if len(s.cache) > 100000 {
			clear(s.cache)
		}
		s.cache[chain] = cached{binding: b, until: now.Add(s.cfg.GateCacheTTL)}
		s.cacheMu.Unlock()
	}
	return b, nil
}

// forget drops chains from the gate's cache after their binding changed.
func (s *Service) forget(chains ...string) {
	s.cacheMu.Lock()
	for _, c := range chains {
		delete(s.cache, strings.ToLower(c))
	}
	s.cacheMu.Unlock()
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func fail(w http.ResponseWriter, status int, msg string) {
	writeJSON(w, status, map[string]string{"error": msg})
}

// User is a signed-in user as the API shows it.
type User struct {
	Name  string `json:"name"`
	Email string `json:"email"`
}

func apiUser(u datastore.OIDCUser) *User {
	name := u.Name
	if name == "" {
		name = u.Email
	}
	return &User{Name: name, Email: u.Email}
}

func (s *Service) info(w http.ResponseWriter, _ *http.Request) {
	if !s.Enabled() {
		writeJSON(w, http.StatusOK, map[string]any{"login": nil})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"login": map[string]string{"type": "oidc", "name": s.ProviderName()},
	})
}

// chainOf checks the request's sync token (never gated): the chain ID,
// lower-case, or "" after answering 401.
func chainOf(w http.ResponseWriter, r *http.Request) string {
	chain, err := auth.Authorize(r)
	if err != nil || chain == "" {
		fail(w, http.StatusUnauthorized, "invalid sync token")
		return ""
	}
	return strings.ToLower(chain)
}

func (s *Service) account(w http.ResponseWriter, r *http.Request) {
	chain := chainOf(w, r)
	if chain == "" {
		return
	}
	out := map[string]any{"login_required": false, "user": nil, "expires": 0}
	if s.Enabled() {
		b, err := s.cfg.Store.ChainBinding(r.Context(), chain)
		if err != nil {
			fail(w, http.StatusInternalServerError, "unable to complete request")
			return
		}
		if b.Valid(s.cfg.Issuer, s.cfg.Now()) {
			out["user"] = apiUser(b.User)
			if !b.SessionExpires.IsZero() {
				out["expires"] = b.SessionExpires.UnixMilli()
			}
		} else {
			out["login_required"] = true
		}
	}
	writeJSON(w, http.StatusOK, out)
}

func (s *Service) signOut(w http.ResponseWriter, r *http.Request) {
	chain := chainOf(w, r)
	if chain == "" {
		return
	}
	if s.Enabled() {
		if err := s.cfg.Store.UnbindChain(r.Context(), chain); err != nil {
			fail(w, http.StatusInternalServerError, "unable to complete request")
			return
		}
		s.forget(chain)
	}
	w.WriteHeader(http.StatusNoContent)
}

func (s *Service) link(w http.ResponseWriter, r *http.Request) {
	chain, err := s.Authorize()(r)
	if err != nil || chain == "" {
		if !auth.WriteGateError(w, err) {
			fail(w, http.StatusUnauthorized, "invalid sync token")
		}
		return
	}
	other, err := auth.AuthenticateToken(r.Header.Get(LinkedTokenHeader))
	if err != nil {
		fail(w, http.StatusBadRequest, "invalid "+LinkedTokenHeader)
		return
	}
	if s.Enabled() {
		ok, err := s.cfg.Store.LinkChain(r.Context(), chain, other, s.cfg.Now())
		if err != nil {
			fail(w, http.StatusInternalServerError, "unable to complete request")
			return
		}
		if !ok { // Unbound since the gate looked.
			auth.WriteLoginRequired(w)
			return
		}
		s.forget(other)
	}
	w.WriteHeader(http.StatusNoContent)
}
