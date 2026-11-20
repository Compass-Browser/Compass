package oidclogin

import (
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
)

// Config is the sign-in configuration (COMPASS_SYNC_OIDC_*, ConfigFromEnv)
// and the service's limits (DefaultConfig). Sign-in is off when Issuer is
// empty.
type Config struct {
	Issuer          string   // COMPASS_SYNC_OIDC_ISSUER
	ClientID        string   // COMPASS_SYNC_OIDC_CLIENT_ID
	ClientSecret    string   // COMPASS_SYNC_OIDC_CLIENT_SECRET or _CLIENT_SECRET_FILE
	Name            string   // COMPASS_SYNC_OIDC_NAME, default the issuer's host
	Scopes          []string // COMPASS_SYNC_OIDC_SCOPES
	AllowedDomains  []string // COMPASS_SYNC_OIDC_ALLOWED_DOMAINS, lower-case
	AllowedGroups   []string // COMPASS_SYNC_OIDC_ALLOWED_GROUPS
	GroupsClaim     string   // COMPASS_SYNC_OIDC_GROUPS_CLAIM
	SessionDays     int      // COMPASS_SYNC_OIDC_SESSION_DAYS: bindings without a refresh token
	RevalidateEvery time.Duration
	// DevFakeIDP serves fakeidp at /dev/fake-idp and signs in against it
	// (COMPASS_SYNC_DEV_FAKE_OIDC=1, localhost only).
	DevFakeIDP bool

	// PublicHost is COMPASS_SYNC_PUBLIC_HOST: the redirect URI is
	// https://<PublicHost>/v2/compass/login/callback (http for localhost).
	PublicHost string

	PendingTTL     time.Duration // A login request's life.
	PollTimeout    time.Duration // How long a poll waits.
	GateCacheTTL   time.Duration // How long a chain's binding is cached.
	RevalidateTick time.Duration // How often the revalidation loop looks.
	LoginsPerChain int           // Login requests per chain per LoginWindow.
	LoginsPerIP    int           // Login requests per IP per LoginWindow.
	LoginWindow    time.Duration
	MaxPending     int // All chains together.

	// Store keeps users and bindings (datastore.SQLite).
	Store Store
	// ClientIP returns the client address used for rate limits.
	ClientIP func(*http.Request) string
	// HTTPClient talks to the identity provider (nil: http.DefaultClient).
	HTTPClient *http.Client
	// Now is the clock for login requests, sessions and revalidation.
	Now func() time.Time
}

// Defaults (sync.md, "Company sign-in (OIDC)").
const (
	DefaultScopes          = "openid email profile offline_access"
	DefaultGroupsClaim     = "groups"
	DefaultSessionDays     = 30
	DefaultRevalidateHours = 24
	// FakeIDPPath is where COMPASS_SYNC_DEV_FAKE_OIDC serves the fake
	// identity provider.
	FakeIDPPath = "/dev/fake-idp"
	// FakeIDPClientID is the fake identity provider's client ID.
	FakeIDPClientID = "compass-dev"
)

// DefaultConfig returns the defaults, sign-in off.
func DefaultConfig() Config {
	return Config{
		Scopes:          strings.Fields(DefaultScopes),
		GroupsClaim:     DefaultGroupsClaim,
		SessionDays:     DefaultSessionDays,
		RevalidateEvery: DefaultRevalidateHours * time.Hour,
		PendingTTL:      10 * time.Minute,
		PollTimeout:     30 * time.Second,
		GateCacheTTL:    10 * time.Second,
		RevalidateTick:  15 * time.Minute,
		LoginsPerChain:  10,
		LoginsPerIP:     30,
		LoginWindow:     10 * time.Minute,
		MaxPending:      10000,
		Now:             time.Now,
	}
}

// Enabled says whether sign-in is on.
func (c Config) Enabled() bool { return c.Issuer != "" }

func list(v string, lower bool) []string {
	var out []string
	for _, s := range strings.Split(v, ",") {
		if s = strings.TrimSpace(s); s != "" {
			if lower {
				s = strings.ToLower(s)
			}
			out = append(out, s)
		}
	}
	return out
}

// IsLocalHost says whether |host| (maybe with a port) is this machine:
// localhost, 127.0.0.1 or ::1.
func IsLocalHost(host string) bool {
	if h, _, err := net.SplitHostPort(host); err == nil {
		host = h
	}
	host = strings.Trim(strings.ToLower(host), "[]")
	return host == "localhost" || host == "127.0.0.1" || host == "::1"
}

// BaseURL is the server's public origin: https://<host>, or http:// for a
// localhost host (development).
func BaseURL(publicHost string) string {
	if IsLocalHost(publicHost) {
		return "http://" + publicHost
	}
	return "https://" + publicHost
}

// ConfigFromEnv reads COMPASS_SYNC_OIDC_* (and COMPASS_SYNC_DEV_FAKE_OIDC)
// onto DefaultConfig, for a server reachable at |publicHost|
// (COMPASS_SYNC_PUBLIC_HOST). It refuses an issuer without a client ID or
// a public host.
func ConfigFromEnv(publicHost string) (Config, error) {
	cfg := DefaultConfig()
	cfg.PublicHost = publicHost
	cfg.Issuer = strings.TrimSpace(os.Getenv("COMPASS_SYNC_OIDC_ISSUER"))
	cfg.ClientID = strings.TrimSpace(os.Getenv("COMPASS_SYNC_OIDC_CLIENT_ID"))
	cfg.Name = strings.TrimSpace(os.Getenv("COMPASS_SYNC_OIDC_NAME"))
	cfg.ClientSecret = os.Getenv("COMPASS_SYNC_OIDC_CLIENT_SECRET")
	if path := os.Getenv("COMPASS_SYNC_OIDC_CLIENT_SECRET_FILE"); path != "" {
		if cfg.ClientSecret != "" {
			return cfg, errors.New("set COMPASS_SYNC_OIDC_CLIENT_SECRET or _CLIENT_SECRET_FILE, not both")
		}
		b, err := os.ReadFile(path) //nolint:gosec // G304: the admin's path
		if err != nil {
			return cfg, fmt.Errorf("reading COMPASS_SYNC_OIDC_CLIENT_SECRET_FILE: %w", err)
		}
		cfg.ClientSecret = strings.TrimSpace(string(b))
	}
	if v := os.Getenv("COMPASS_SYNC_OIDC_SCOPES"); v != "" {
		cfg.Scopes = strings.Fields(strings.ReplaceAll(v, ",", " "))
	}
	cfg.AllowedDomains = list(os.Getenv("COMPASS_SYNC_OIDC_ALLOWED_DOMAINS"), true)
	for i, d := range cfg.AllowedDomains {
		cfg.AllowedDomains[i] = strings.TrimPrefix(d, "@")
	}
	cfg.AllowedGroups = list(os.Getenv("COMPASS_SYNC_OIDC_ALLOWED_GROUPS"), false)
	if v := strings.TrimSpace(os.Getenv("COMPASS_SYNC_OIDC_GROUPS_CLAIM")); v != "" {
		cfg.GroupsClaim = v
	}
	if v := os.Getenv("COMPASS_SYNC_OIDC_SESSION_DAYS"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 1 || n > 3650 {
			return cfg, fmt.Errorf("invalid COMPASS_SYNC_OIDC_SESSION_DAYS %q (days, 1 or more)", v)
		}
		cfg.SessionDays = n
	}
	if v := os.Getenv("COMPASS_SYNC_OIDC_REVALIDATE_HOURS"); v != "" {
		n, err := strconv.Atoi(v)
		if err != nil || n < 0 || n > 24*365 {
			return cfg, fmt.Errorf("invalid COMPASS_SYNC_OIDC_REVALIDATE_HOURS %q (hours, 0 = off)", v)
		}
		cfg.RevalidateEvery = time.Duration(n) * time.Hour
	}
	cfg.DevFakeIDP, _ = strconv.ParseBool(os.Getenv("COMPASS_SYNC_DEV_FAKE_OIDC"))
	if cfg.DevFakeIDP {
		if !IsLocalHost(publicHost) {
			return cfg, errors.New("COMPASS_SYNC_DEV_FAKE_OIDC needs COMPASS_SYNC_PUBLIC_HOST " +
				"to be localhost or 127.0.0.1 (with the port), it's for development only")
		}
		if cfg.Issuer != "" {
			return cfg, errors.New("COMPASS_SYNC_DEV_FAKE_OIDC replaces COMPASS_SYNC_OIDC_ISSUER: set one")
		}
		cfg.Issuer = BaseURL(publicHost) + FakeIDPPath
		if cfg.ClientID == "" {
			cfg.ClientID = FakeIDPClientID
		}
		if cfg.Name == "" {
			cfg.Name = "Fake IdP (development)"
		}
	}
	if !cfg.Enabled() {
		return cfg, nil
	}
	if cfg.ClientID == "" {
		return cfg, errors.New("COMPASS_SYNC_OIDC_ISSUER needs COMPASS_SYNC_OIDC_CLIENT_ID")
	}
	if publicHost == "" {
		return cfg, errors.New("COMPASS_SYNC_OIDC_ISSUER needs COMPASS_SYNC_PUBLIC_HOST " +
			"(the redirect URI is https://<public host>/v2/compass/login/callback)")
	}
	return cfg, nil
}
