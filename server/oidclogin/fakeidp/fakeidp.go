// Package fakeidp is an OpenID Connect provider for tests and development
// (COMPASS_SYNC_DEV_FAKE_OIDC): discovery, JWKS, an authorize page where
// you type any email and name, and a token endpoint (authorization code
// with PKCE, refresh tokens). Nothing is checked but PKCE and the client
// ID: never expose it.
//
// It serves the paths under its issuer's path, so it can be mounted on
// another server (/dev/fake-idp) or served on its own; Client talks to it
// in-process, so the relying party doesn't need to reach it over the
// network.
package fakeidp

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"html/template"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"time"

	jose "github.com/go-jose/go-jose/v4"
)

// Identity is who signs in.
type Identity struct {
	Subject       string // Default: derived from Email.
	Email         string
	Name          string
	EmailVerified bool
	Groups        []string
}

type grant struct {
	who                        Identity
	nonce, challenge, redirect string
	expires                    time.Time
}

// IDP is the fake provider.
type IDP struct {
	issuer   string
	path     string // The issuer's path, served under.
	clientID string
	key      *rsa.PrivateKey
	signer   jose.Signer

	mu      sync.Mutex
	codes   map[string]grant
	refresh map[string]Identity // Refresh token → who.
	down    bool
	nonce   string // Overrides the nonce of the next ID tokens if set.
	noRT    bool

	// Now is the clock for token times.
	Now func() time.Time
}

const kid = "fake-1"

// New returns a provider whose issuer is |issuer| (e.g.
// http://localhost:8295/dev/fake-idp), for client |clientID|.
func New(issuer, clientID string) *IDP {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		panic(err)
	}
	signer, err := jose.NewSigner(jose.SigningKey{Algorithm: jose.RS256, Key: key},
		(&jose.SignerOptions{}).WithType("JWT").WithHeader("kid", kid))
	if err != nil {
		panic(err)
	}
	issuer = strings.TrimSuffix(issuer, "/")
	u, err := url.Parse(issuer)
	if err != nil {
		panic(err)
	}
	return &IDP{
		issuer: issuer, path: u.Path, clientID: clientID, key: key, signer: signer,
		codes: map[string]grant{}, refresh: map[string]Identity{}, Now: time.Now,
	}
}

// Issuer is the issuer URL.
func (i *IDP) Issuer() string { return i.issuer }

// Client is an HTTP client that reaches the provider in-process (other
// hosts go over the network).
func (i *IDP) Client() *http.Client {
	return &http.Client{Transport: roundTripper{i}, Timeout: 30 * time.Second}
}

type roundTripper struct{ i *IDP }

func (rt roundTripper) RoundTrip(r *http.Request) (*http.Response, error) {
	iu, _ := url.Parse(rt.i.issuer)
	if r.URL.Host != iu.Host || !strings.HasPrefix(r.URL.Path, iu.Path) {
		return http.DefaultTransport.RoundTrip(r)
	}
	rec := httptest.NewRecorder()
	rt.i.ServeHTTP(rec, r)
	resp := rec.Result()
	resp.Request = r
	return resp, nil
}

// SetDown makes the token endpoint answer 503 (an unreachable provider).
func (i *IDP) SetDown(down bool) {
	i.mu.Lock()
	i.down = down
	i.mu.Unlock()
}

// SetNonce makes the next ID tokens carry |nonce| instead of the one asked
// for ("" to stop).
func (i *IDP) SetNonce(nonce string) {
	i.mu.Lock()
	i.nonce = nonce
	i.mu.Unlock()
}

// SetNoRefreshTokens stops issuing refresh tokens (a provider without
// offline_access).
func (i *IDP) SetNoRefreshTokens(no bool) {
	i.mu.Lock()
	i.noRT = no
	i.mu.Unlock()
}

// RevokeUser invalidates the refresh tokens of |subjectOrEmail|: the next
// refresh is invalid_grant.
func (i *IDP) RevokeUser(subjectOrEmail string) {
	i.mu.Lock()
	defer i.mu.Unlock()
	for t, who := range i.refresh {
		if who.Subject == subjectOrEmail || who.Email == subjectOrEmail {
			delete(i.refresh, t)
		}
	}
}

// SetGroups changes the groups of |subjectOrEmail| in its refresh tokens
// (what the next refreshed ID token says).
func (i *IDP) SetGroups(subjectOrEmail string, groups []string) {
	i.mu.Lock()
	defer i.mu.Unlock()
	for t, who := range i.refresh {
		if who.Subject == subjectOrEmail || who.Email == subjectOrEmail {
			who.Groups = groups
			i.refresh[t] = who
		}
	}
}

func random() string {
	b := make([]byte, 24)
	_, _ = rand.Read(b)
	return base64.RawURLEncoding.EncodeToString(b)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func oauthError(w http.ResponseWriter, status int, code, desc string) {
	writeJSON(w, status, map[string]string{"error": code, "error_description": desc})
}

func (i *IDP) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	switch strings.TrimPrefix(r.URL.Path, i.path) {
	case "/.well-known/openid-configuration":
		writeJSON(w, http.StatusOK, map[string]any{
			"issuer":                                i.issuer,
			"authorization_endpoint":                i.issuer + "/authorize",
			"token_endpoint":                        i.issuer + "/token",
			"jwks_uri":                              i.issuer + "/keys",
			"response_types_supported":              []string{"code"},
			"subject_types_supported":               []string{"public"},
			"id_token_signing_alg_values_supported": []string{"RS256"},
			"code_challenge_methods_supported":      []string{"S256"},
			"scopes_supported":                      []string{"openid", "email", "profile", "offline_access"},
		})
	case "/keys":
		writeJSON(w, http.StatusOK, jose.JSONWebKeySet{Keys: []jose.JSONWebKey{{
			Key: &i.key.PublicKey, KeyID: kid, Algorithm: string(jose.RS256), Use: "sig",
		}}})
	case "/authorize":
		if r.Method == http.MethodPost {
			i.authorizePost(w, r)
		} else {
			i.authorizeForm(w, r)
		}
	case "/token":
		i.token(w, r)
	default:
		http.NotFound(w, r)
	}
}

var form = template.Must(template.New("form").Parse(`<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fake IdP</title>
<style>body{font:16px/1.4 system-ui,sans-serif;max-width:380px;margin:40px auto;padding:0 16px}
label{display:block;margin:12px 0 4px}input[type=text],input[type=email]{width:100%;padding:8px;box-sizing:border-box}
button{margin-top:16px;padding:8px 16px}p{color:#777;font-size:14px}</style></head><body>
<h1>Fake IdP</h1><p>Development only: type any identity.</p>
<form method="post">
{{range $k, $v := .Hidden}}<input type="hidden" name="{{$k}}" value="{{$v}}">{{end}}
<label for="email">Email</label><input id="email" name="email" type="email" required value="dev@example.com">
<label for="name">Name</label><input id="name" name="name" type="text" value="Dev User">
<label for="groups">Groups (comma-separated)</label><input id="groups" name="groups" type="text" value="">
<label><input name="email_verified" type="checkbox" value="true" checked> Email verified</label>
<button type="submit">Sign in</button>
</form></body></html>`))

var passed = []string{"client_id", "redirect_uri", "state", "nonce", "code_challenge", "code_challenge_method", "response_type", "scope"}

func (i *IDP) authorizeForm(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	if q.Get("client_id") != i.clientID || q.Get("redirect_uri") == "" {
		http.Error(w, "unknown client_id or no redirect_uri", http.StatusBadRequest)
		return
	}
	hidden := map[string]string{}
	for _, k := range passed {
		hidden[k] = q.Get(k)
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'")
	_ = form.Execute(w, map[string]any{"Hidden": hidden})
}

func (i *IDP) authorizePost(w http.ResponseWriter, r *http.Request) {
	if err := r.ParseForm(); err != nil {
		http.Error(w, "bad form", http.StatusBadRequest)
		return
	}
	who := Identity{
		Email:         strings.TrimSpace(r.PostForm.Get("email")),
		Name:          strings.TrimSpace(r.PostForm.Get("name")),
		EmailVerified: r.PostForm.Get("email_verified") == "true",
	}
	for _, g := range strings.Split(r.PostForm.Get("groups"), ",") {
		if g = strings.TrimSpace(g); g != "" {
			who.Groups = append(who.Groups, g)
		}
	}
	redirect, err := i.Approve(r.PostForm, who)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	http.Redirect(w, r, redirect, http.StatusFound)
}

// Approve is the authorize page's submit, for tests: for the authorize
// request's parameters |q| (from the URL the relying party redirected to),
// it signs |who| in and returns the redirect back, with the code.
func (i *IDP) Approve(q url.Values, who Identity) (string, error) {
	if q.Get("client_id") != i.clientID {
		return "", fmt.Errorf("unknown client_id %q", q.Get("client_id"))
	}
	if q.Get("code_challenge_method") != "S256" || q.Get("code_challenge") == "" {
		return "", fmt.Errorf("PKCE S256 required")
	}
	redirect, err := url.Parse(q.Get("redirect_uri"))
	if err != nil || q.Get("redirect_uri") == "" {
		return "", fmt.Errorf("bad redirect_uri")
	}
	if who.Subject == "" {
		sum := sha256.Sum256([]byte(strings.ToLower(who.Email)))
		who.Subject = "fake-" + base64.RawURLEncoding.EncodeToString(sum[:9])
	}
	code := random()
	i.mu.Lock()
	i.codes[code] = grant{who: who, nonce: q.Get("nonce"), challenge: q.Get("code_challenge"),
		redirect: q.Get("redirect_uri"), expires: i.Now().Add(time.Minute)}
	i.mu.Unlock()
	rq := redirect.Query()
	rq.Set("code", code)
	rq.Set("state", q.Get("state"))
	redirect.RawQuery = rq.Encode()
	return redirect.String(), nil
}

func (i *IDP) clientOK(r *http.Request) bool {
	if id, _, ok := r.BasicAuth(); ok {
		id, _ = url.QueryUnescape(id)
		return id == i.clientID
	}
	return r.PostForm.Get("client_id") == i.clientID
}

func (i *IDP) token(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost || r.ParseForm() != nil {
		oauthError(w, http.StatusBadRequest, "invalid_request", "POST a form")
		return
	}
	i.mu.Lock()
	defer i.mu.Unlock()
	if i.down {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
		return
	}
	if !i.clientOK(r) {
		oauthError(w, http.StatusUnauthorized, "invalid_client", "unknown client")
		return
	}
	var who Identity
	nonce := ""
	switch r.PostForm.Get("grant_type") {
	case "authorization_code":
		g, ok := i.codes[r.PostForm.Get("code")]
		delete(i.codes, r.PostForm.Get("code"))
		if !ok || !i.Now().Before(g.expires) || g.redirect != r.PostForm.Get("redirect_uri") {
			oauthError(w, http.StatusBadRequest, "invalid_grant", "unknown or expired code")
			return
		}
		sum := sha256.Sum256([]byte(r.PostForm.Get("code_verifier")))
		if base64.RawURLEncoding.EncodeToString(sum[:]) != g.challenge {
			oauthError(w, http.StatusBadRequest, "invalid_grant", "PKCE verification failed")
			return
		}
		who, nonce = g.who, g.nonce
	case "refresh_token":
		rt := r.PostForm.Get("refresh_token")
		var ok bool
		if who, ok = i.refresh[rt]; !ok {
			oauthError(w, http.StatusBadRequest, "invalid_grant", "refresh token revoked")
			return
		}
		delete(i.refresh, rt) // Rotated.
	default:
		oauthError(w, http.StatusBadRequest, "unsupported_grant_type", "")
		return
	}
	if i.nonce != "" {
		nonce = i.nonce
	}
	idToken, err := i.idToken(who, nonce)
	if err != nil {
		oauthError(w, http.StatusInternalServerError, "server_error", err.Error())
		return
	}
	out := map[string]any{
		"access_token": random(),
		"token_type":   "Bearer",
		"expires_in":   300,
		"id_token":     idToken,
	}
	if !i.noRT {
		rt := random()
		i.refresh[rt] = who
		out["refresh_token"] = rt
	}
	writeJSON(w, http.StatusOK, out)
}

func (i *IDP) idToken(who Identity, nonce string) (string, error) {
	now := i.Now()
	c := map[string]any{
		"iss":            i.issuer,
		"sub":            who.Subject,
		"aud":            i.clientID,
		"iat":            now.Unix(),
		"exp":            now.Add(time.Hour).Unix(),
		"email":          who.Email,
		"email_verified": who.EmailVerified,
		"name":           who.Name,
		"groups":         who.Groups,
	}
	if who.Groups == nil {
		c["groups"] = []string{}
	}
	if nonce != "" {
		c["nonce"] = nonce
	}
	payload, err := json.Marshal(c)
	if err != nil {
		return "", err
	}
	sig, err := i.signer.Sign(payload)
	if err != nil {
		return "", err
	}
	return sig.CompactSerialize()
}
