package oidclogin

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/url"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/coreos/go-oidc/v3/oidc"
	"github.com/go-chi/chi/v5"
	"github.com/rs/zerolog/log"
	"golang.org/x/oauth2"

	"github.com/brave/go-sync/datastore"
)

// Login request states, as the poll answers them.
const (
	StatusPending = "pending"
	StatusDone    = "done"
	StatusDenied  = "denied"
	StatusExpired = "expired"
)

// How the browser leaves the done page: a tab the client closes, or a
// redirect to compass://sync-login-done (iOS, ASWebAuthenticationSession).
const (
	ReturnTab     = "tab"
	ReturnCompass = "compass"
)

// CompassReturnURL is where return=compass sends the browser, with
// ?status=done|denied.
const CompassReturnURL = "compass://sync-login-done"

const cookieName = "compass_login"

// userCodeAlphabet has no 0/O, 1/I/L: codes are read and compared by eye.
const userCodeAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"

// identity is who the identity provider said signed in.
type identity struct {
	subject, email, name string
	refreshToken         string
}

type pending struct {
	id       string
	chain    string // Lower-case.
	device   string
	ret      string
	userCode string
	created  time.Time
	expires  time.Time
	status   string
	user     *User
	changed  chan struct{} // Closed (and replaced) when status changes.

	// Set by /start.
	state, nonce, verifier string
	browser                [32]byte // SHA-256 of the cookie /start set.
	// Set by the callback: the consent page is up.
	who  *identity
	csrf string
}

func randomString(n int) string {
	b := make([]byte, n)
	_, _ = rand.Read(b) // Never fails (crypto/rand).
	return base64.RawURLEncoding.EncodeToString(b)
}

func userCode() string {
	b := make([]byte, 6)
	_, _ = rand.Read(b)
	out := make([]byte, 0, 7)
	for i, c := range b {
		if i == 3 {
			out = append(out, '-')
		}
		// 256 % 31 leaves a slight bias; fine for a comparison code.
		out = append(out, userCodeAlphabet[int(c)%len(userCodeAlphabet)])
	}
	return string(out)
}

func (p *pending) statusAt(now time.Time) string {
	if p.status == StatusPending && !now.Before(p.expires) {
		return StatusExpired
	}
	return p.status
}

func (p *pending) setStatus(status string, user *User) {
	p.status, p.user = status, user
	close(p.changed)
	p.changed = make(chan struct{})
}

// sweepLocked drops logins a TTL past their expiry (a finished one stays
// that long for the poll) and old rate-limit entries, at most once a minute.
func (s *Service) sweepLocked(now time.Time) {
	if now.Sub(s.lastSweep) < time.Minute {
		return
	}
	s.lastSweep = now
	for id, p := range s.logins {
		if !now.Before(p.expires.Add(s.cfg.PendingTTL)) {
			delete(s.logins, id)
			if p.state != "" {
				delete(s.byState, p.state)
			}
		}
	}
	for _, m := range []map[string][]time.Time{s.byChain, s.byIP} {
		for k, times := range m {
			if kept := s.recent(times, now); len(kept) == 0 {
				delete(m, k)
			} else {
				m[k] = kept
			}
		}
	}
}

func (s *Service) recent(times []time.Time, now time.Time) []time.Time {
	cutoff := now.Add(-s.cfg.LoginWindow)
	i := 0
	for i < len(times) && !times[i].After(cutoff) {
		i++
	}
	return times[i:]
}

// allowLocked records an attempt by |key| in |m| and says whether it is
// within |limit| per window (as the pairing relay limits).
func (s *Service) allowLocked(m map[string][]time.Time, key string, limit int, now time.Time) bool {
	times := s.recent(m[key], now)
	if len(times) >= limit {
		m[key] = times
		return false
	}
	m[key] = append(times, now)
	return true
}

type createBody struct {
	DeviceName string `json:"device_name"`
	Return     string `json:"return"`
}

func cleanDeviceName(v string) string {
	v = strings.Map(func(r rune) rune {
		if r < 0x20 || r == 0x7f {
			return -1
		}
		return r
	}, strings.TrimSpace(v))
	if utf8.RuneCountInString(v) > 80 {
		v = string([]rune(v)[:80])
	}
	if v == "" {
		v = "Compass"
	}
	return v
}

func (s *Service) create(w http.ResponseWriter, r *http.Request) {
	if !s.Enabled() {
		fail(w, http.StatusNotFound, "login_disabled")
		return
	}
	chain := chainOf(w, r)
	if chain == "" {
		return
	}
	var body createBody
	raw, err := io.ReadAll(io.LimitReader(r.Body, 4096))
	if err == nil && len(raw) > 0 {
		err = json.Unmarshal(raw, &body)
	}
	if err != nil {
		fail(w, http.StatusBadRequest, `body must be JSON {"device_name", "return"}`)
		return
	}
	switch body.Return {
	case "":
		body.Return = ReturnTab
	case ReturnTab, ReturnCompass:
	default:
		fail(w, http.StatusBadRequest, `"return" must be "tab" or "compass"`)
		return
	}
	if !s.Ready() {
		fail(w, http.StatusServiceUnavailable, "login_unavailable")
		return
	}
	ip := s.cfg.ClientIP(r)

	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.cfg.Now()
	s.sweepLocked(now)
	if !s.allowLocked(s.byIP, ip, s.cfg.LoginsPerIP, now) ||
		!s.allowLocked(s.byChain, chain, s.cfg.LoginsPerChain, now) {
		fail(w, http.StatusTooManyRequests, "too many login requests")
		return
	}
	if len(s.logins) >= s.cfg.MaxPending {
		fail(w, http.StatusServiceUnavailable, "too many login requests")
		return
	}
	p := &pending{
		id:       randomString(18),
		chain:    chain,
		device:   cleanDeviceName(body.DeviceName),
		ret:      body.Return,
		userCode: userCode(),
		created:  now,
		expires:  now.Add(s.cfg.PendingTTL),
		status:   StatusPending,
		changed:  make(chan struct{}),
	}
	s.logins[p.id] = p
	writeJSON(w, http.StatusOK, map[string]any{
		"id":        p.id,
		"url":       BaseURL(s.cfg.PublicHost) + "/v2/compass/login/" + p.id + "/start",
		"user_code": p.userCode,
		"expires":   p.expires.UnixMilli(),
	})
}

func (s *Service) poll(w http.ResponseWriter, r *http.Request) {
	if !s.Enabled() {
		fail(w, http.StatusNotFound, "login_disabled")
		return
	}
	chain := chainOf(w, r)
	if chain == "" {
		return
	}
	id := chi.URLParam(r, "id")
	wait := r.URL.Query().Get("wait") != "0"
	deadline := time.NewTimer(s.cfg.PollTimeout)
	defer deadline.Stop()
	for {
		s.mu.Lock()
		now := s.cfg.Now()
		p := s.logins[id]
		if p == nil || p.chain != chain {
			s.mu.Unlock()
			fail(w, http.StatusNotFound, "not_found")
			return
		}
		status, user, changed, left := p.statusAt(now), p.user, p.changed, p.expires.Sub(now)
		s.mu.Unlock()
		if status != StatusPending || !wait {
			writeJSON(w, http.StatusOK, map[string]any{"status": status, "user": user})
			return
		}
		expiry := time.NewTimer(left)
		select {
		case <-changed:
		case <-expiry.C:
		case <-deadline.C:
			wait = false
		case <-r.Context().Done():
			expiry.Stop()
			return
		}
		expiry.Stop()
	}
}

// lookup returns the login |id| if it is still pending at |now|.
func (s *Service) lookupLocked(id string, now time.Time) *pending {
	p := s.logins[id]
	if p == nil || p.statusAt(now) != StatusPending {
		return nil
	}
	return p
}

func (s *Service) start(w http.ResponseWriter, r *http.Request) {
	if !s.Enabled() || !s.Ready() {
		s.errorPage(w, http.StatusNotFound, "Sign-in isn't available on this server right now.")
		return
	}
	secret := randomString(32)
	s.mu.Lock()
	p := s.lookupLocked(chi.URLParam(r, "id"), s.cfg.Now())
	if p == nil {
		s.mu.Unlock()
		s.errorPage(w, http.StatusGone, "This sign-in link has expired or was already used. Start again from Compass.")
		return
	}
	if p.state != "" {
		delete(s.byState, p.state)
	}
	p.state, p.nonce, p.verifier = randomString(24), randomString(24), oauth2.GenerateVerifier()
	p.browser = sha256.Sum256([]byte(secret))
	p.who, p.csrf = nil, ""
	s.byState[p.state] = p.id
	authURL := s.oauth.AuthCodeURL(p.state, oidc.Nonce(p.nonce), oauth2.S256ChallengeOption(p.verifier))
	s.mu.Unlock()

	http.SetCookie(w, &http.Cookie{
		Name:     cookieName,
		Value:    secret,
		Path:     "/v2/compass/login",
		MaxAge:   int(s.cfg.PendingTTL / time.Second),
		HttpOnly: true,
		Secure:   !IsLocalHost(s.cfg.PublicHost),
		SameSite: http.SameSiteLaxMode,
	})
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Referrer-Policy", "no-referrer")
	http.Redirect(w, r, authURL, http.StatusFound)
}

// sameBrowser says whether the request carries the cookie /start set for
// |p|.
func sameBrowser(r *http.Request, p *pending) bool {
	c, err := r.Cookie(cookieName)
	if err != nil {
		return false
	}
	h := sha256.Sum256([]byte(c.Value))
	return subtle.ConstantTimeCompare(h[:], p.browser[:]) == 1
}

// errDenied is a sign-in the server's policy refuses; its message is shown.
type errDenied struct{ msg string }

func (e errDenied) Error() string { return e.msg }

// claims are the ID token claims sign-in reads; the groups claim is read
// separately (its name is configurable).
type claims struct {
	Subject           string          `json:"sub"`
	Email             string          `json:"email"`
	EmailVerified     json.RawMessage `json:"email_verified"`
	Name              string          `json:"name"`
	PreferredUsername string          `json:"preferred_username"`
	Nonce             string          `json:"nonce"`
}

func truthy(raw json.RawMessage) bool {
	var b bool
	if json.Unmarshal(raw, &b) == nil {
		return b
	}
	var str string
	return json.Unmarshal(raw, &str) == nil && strings.EqualFold(str, "true")
}

// groupsOf reads a groups claim: a list of strings, or one string.
func groupsOf(v any) []string {
	switch g := v.(type) {
	case string:
		return []string{g}
	case []any:
		var out []string
		for _, x := range g {
			if s, ok := x.(string); ok {
				out = append(out, s)
			}
		}
		return out
	}
	return nil
}

// check applies the server's policy to a verified ID token: email domain
// (with email_verified) and groups. Returns who signed in.
func (s *Service) check(tok *oidc.IDToken) (identity, error) {
	var c claims
	if err := tok.Claims(&c); err != nil {
		return identity{}, err
	}
	who := identity{subject: tok.Subject, email: c.Email, name: c.Name}
	if who.email == "" {
		who.email = c.PreferredUsername
	}
	if who.name == "" {
		who.name = c.PreferredUsername
	}
	if len(s.cfg.AllowedDomains) > 0 {
		at := strings.LastIndexByte(c.Email, '@')
		domain := strings.ToLower(c.Email[at+1:])
		if at < 0 || !truthy(c.EmailVerified) {
			return who, errDenied{"Your account has no verified email address, which this server requires."}
		}
		ok := false
		for _, d := range s.cfg.AllowedDomains {
			if domain == d {
				ok = true
				break
			}
		}
		if !ok {
			return who, errDenied{"Accounts of " + domain + " can't use this sync server."}
		}
	}
	if len(s.cfg.AllowedGroups) > 0 {
		var all map[string]any
		if err := tok.Claims(&all); err != nil {
			return who, err
		}
		ok := false
		for _, g := range groupsOf(all[s.cfg.GroupsClaim]) {
			for _, a := range s.cfg.AllowedGroups {
				if g == a {
					ok = true
				}
			}
		}
		if !ok {
			return who, errDenied{"Your account isn't in a group allowed to use this sync server."}
		}
	}
	return who, nil
}

func (s *Service) callback(w http.ResponseWriter, r *http.Request) {
	if !s.Enabled() || !s.Ready() {
		s.errorPage(w, http.StatusNotFound, "Sign-in isn't available on this server right now.")
		return
	}
	q := r.URL.Query()
	state := q.Get("state")
	expired := "This sign-in has expired or was already used. Start again from Compass."

	s.mu.Lock()
	id := s.byState[state]
	p := s.lookupLocked(id, s.cfg.Now())
	if state == "" || p == nil || p.state != state {
		s.mu.Unlock()
		s.errorPage(w, http.StatusBadRequest, expired)
		return
	}
	if !sameBrowser(r, p) {
		s.mu.Unlock()
		s.errorPage(w, http.StatusBadRequest,
			"This sign-in was started in another browser. Start again from Compass, in this browser.")
		return
	}
	// The state is single use.
	delete(s.byState, state)
	p.state = ""
	nonce, verifier := p.nonce, p.verifier
	s.mu.Unlock()

	if e := q.Get("error"); e != "" {
		msg := "The identity provider didn't sign you in (" + e + ")."
		if d := q.Get("error_description"); d != "" {
			msg += " " + d
		}
		s.errorPage(w, http.StatusForbidden, msg)
		return
	}
	ctx := s.clientCtx(r.Context())
	tok, err := s.oauth.Exchange(ctx, q.Get("code"), oauth2.VerifierOption(verifier))
	if err != nil {
		log.Warn().Err(err).Msg("Sign-in: code exchange failed")
		s.errorPage(w, http.StatusBadGateway, "The identity provider didn't confirm the sign-in. Try again.")
		return
	}
	raw, _ := tok.Extra("id_token").(string)
	if raw == "" {
		s.errorPage(w, http.StatusBadGateway, "The identity provider sent no ID token.")
		return
	}
	idt, err := s.verifier.Verify(ctx, raw)
	if err != nil {
		log.Warn().Err(err).Msg("Sign-in: ID token rejected")
		s.errorPage(w, http.StatusBadGateway, "The identity provider's answer couldn't be verified.")
		return
	}
	if subtle.ConstantTimeCompare([]byte(idt.Nonce), []byte(nonce)) != 1 {
		log.Warn().Msg("Sign-in: ID token nonce mismatch")
		s.errorPage(w, http.StatusBadRequest, "The identity provider's answer doesn't match this sign-in.")
		return
	}
	who, err := s.check(idt)
	if err != nil {
		var d errDenied
		if errors.As(err, &d) {
			log.Info().Str("email", who.email).Str("reason", d.msg).Msg("Sign-in refused")
			s.errorPage(w, http.StatusForbidden, d.msg)
			return
		}
		s.errorPage(w, http.StatusBadGateway, "The identity provider's answer couldn't be read.")
		return
	}
	revoked, err := s.cfg.Store.OIDCUserRevoked(r.Context(), s.cfg.Issuer, who.subject)
	if err != nil {
		s.errorPage(w, http.StatusInternalServerError, "Something went wrong. Try again.")
		return
	}
	if revoked {
		s.errorPage(w, http.StatusForbidden, "Your access to this sync server was revoked by its administrator.")
		return
	}
	who.refreshToken = tok.RefreshToken

	s.mu.Lock()
	p = s.lookupLocked(id, s.cfg.Now())
	if p == nil {
		s.mu.Unlock()
		s.errorPage(w, http.StatusGone, expired)
		return
	}
	p.who, p.csrf = &who, randomString(24)
	page := consentPage{
		Provider: s.ProviderName(),
		Email:    who.email,
		Name:     who.name,
		Device:   p.device,
		Code:     p.userCode,
		ID:       p.id,
		CSRF:     p.csrf,
	}
	ret := p.ret
	s.mu.Unlock()
	s.render(w, http.StatusOK, "consent", page, ret == ReturnCompass)
}

func (s *Service) approve(w http.ResponseWriter, r *http.Request) { s.decide(w, r, true) }
func (s *Service) deny(w http.ResponseWriter, r *http.Request)    { s.decide(w, r, false) }

func (s *Service) decide(w http.ResponseWriter, r *http.Request, allow bool) {
	if !s.Enabled() || !s.Ready() {
		s.errorPage(w, http.StatusNotFound, "Sign-in isn't available on this server right now.")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 4096)
	csrf := r.PostFormValue("csrf")
	id := chi.URLParam(r, "id")

	s.mu.Lock()
	p := s.lookupLocked(id, s.cfg.Now())
	if p == nil || p.who == nil {
		s.mu.Unlock()
		s.errorPage(w, http.StatusGone, "This sign-in has expired or was already used. Start again from Compass.")
		return
	}
	if !sameBrowser(r, p) || csrf == "" || subtle.ConstantTimeCompare([]byte(csrf), []byte(p.csrf)) != 1 {
		s.mu.Unlock()
		s.errorPage(w, http.StatusForbidden, "This page is out of date. Start again from Compass.")
		return
	}
	who, chain, ret := *p.who, p.chain, p.ret
	// One decision per consent page.
	p.who, p.csrf = nil, ""
	if !allow {
		p.setStatus(StatusDenied, nil)
		s.mu.Unlock()
		s.finish(w, r, ret, StatusDenied)
		return
	}
	s.mu.Unlock()

	now := s.cfg.Now()
	var expires time.Time
	if who.refreshToken == "" {
		expires = now.AddDate(0, 0, s.cfg.SessionDays)
	}
	u, err := s.cfg.Store.SaveOIDCLogin(r.Context(), datastore.OIDCLogin{
		Issuer:       s.cfg.Issuer,
		Subject:      who.subject,
		Email:        who.email,
		Name:         who.name,
		RefreshToken: who.refreshToken,
	}, chain, expires, now)
	if errors.Is(err, datastore.ErrOIDCUserRevoked) {
		s.errorPage(w, http.StatusForbidden, "Your access to this sync server was revoked by its administrator.")
		return
	}
	if err != nil {
		log.Error().Err(err).Msg("Sign-in: saving the binding failed")
		s.errorPage(w, http.StatusInternalServerError, "Something went wrong. Try again.")
		return
	}
	s.forget(chain)
	log.Info().Str("chainID", chain).Str("email", u.Email).Msg("Sign-in: chain bound")
	s.mu.Lock()
	if p = s.logins[id]; p != nil {
		p.setStatus(StatusDone, apiUser(u))
	}
	s.mu.Unlock()
	s.finish(w, r, ret, StatusDone)
}

// finish ends the browser part: the done page, or back to the app.
func (s *Service) finish(w http.ResponseWriter, r *http.Request, ret, status string) {
	http.SetCookie(w, &http.Cookie{
		Name: cookieName, Value: "", Path: "/v2/compass/login", MaxAge: -1,
		HttpOnly: true, Secure: !IsLocalHost(s.cfg.PublicHost), SameSite: http.SameSiteLaxMode,
	})
	if ret == ReturnCompass {
		w.Header().Set("Cache-Control", "no-store")
		http.Redirect(w, r, CompassReturnURL+"?status="+url.QueryEscape(status), http.StatusFound)
		return
	}
	s.render(w, http.StatusOK, "done", donePage{Provider: s.ProviderName(), Allowed: status == StatusDone}, false)
}
