package push

import (
	"bytes"
	"context"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"
)

// FCM delivery for Compass for Android (docs/design/android.md, "Sync and
// push"): FCM HTTP v1, authorized with a service account's OAuth token. The
// message carries no content, only a wake-up: the app syncs and shows the
// sent tab itself, as the iOS notification extension does.

// FCMConfig is the Firebase project's service account.
type FCMConfig struct {
	// CredentialsPath is the service account's JSON key file.
	CredentialsPath string
	// TokenURL and SendURL, if set, replace Google's (tests). SendURL may
	// contain "%s" for the project ID.
	TokenURL string
	SendURL  string
	// HTTPClient, if set, replaces the default client (tests).
	HTTPClient *http.Client
}

// Enabled says whether a service account is configured.
func (c FCMConfig) Enabled() bool { return c.CredentialsPath != "" }

const (
	fcmTokenURL = "https://oauth2.googleapis.com/token"
	fcmSendURL  = "https://fcm.googleapis.com/v1/projects/%s/messages:send"
	fcmScope    = "https://www.googleapis.com/auth/firebase.messaging"
	// FCMEnvironment marks an FCM registration in the tokens table, whose
	// environment is otherwise APNs's "sandbox" or "production".
	FCMEnvironment = "fcm"
)

// serviceAccount is the part of a service account key file used here.
type serviceAccount struct {
	ProjectID   string `json:"project_id"`
	ClientEmail string `json:"client_email"`
	PrivateKey  string `json:"private_key"`
	TokenURI    string `json:"token_uri"`
}

// FCMSender sends FCM messages.
type FCMSender struct {
	cfg     FCMConfig
	account serviceAccount
	key     *rsa.PrivateKey
	client  *http.Client

	mu      sync.Mutex
	token   string
	expires time.Time
	now     func() time.Time
}

// NewFCMSender loads the service account key.
func NewFCMSender(cfg FCMConfig) (*FCMSender, error) {
	raw, err := os.ReadFile(cfg.CredentialsPath)
	if err != nil {
		return nil, fmt.Errorf("reading the FCM service account: %w", err)
	}
	return newFCMSender(cfg, raw)
}

func newFCMSender(cfg FCMConfig, raw []byte) (*FCMSender, error) {
	var account serviceAccount
	if err := json.Unmarshal(raw, &account); err != nil {
		return nil, fmt.Errorf("the FCM service account isn't JSON: %w", err)
	}
	if account.ProjectID == "" || account.ClientEmail == "" || account.PrivateKey == "" {
		return nil, errors.New("the FCM service account needs project_id, client_email and private_key")
	}
	key, err := parseRSAKey([]byte(account.PrivateKey))
	if err != nil {
		return nil, err
	}
	client := cfg.HTTPClient
	if client == nil {
		client = &http.Client{Timeout: 15 * time.Second}
	}
	return &FCMSender{cfg: cfg, account: account, key: key, client: client, now: time.Now}, nil
}

func parseRSAKey(raw []byte) (*rsa.PrivateKey, error) {
	block, _ := pem.Decode(raw)
	if block == nil {
		return nil, errors.New("the FCM private key isn't PEM")
	}
	if key, err := x509.ParsePKCS8PrivateKey(block.Bytes); err == nil {
		if rsaKey, ok := key.(*rsa.PrivateKey); ok {
			return rsaKey, nil
		}
		return nil, errors.New("the FCM private key isn't RSA")
	}
	return x509.ParsePKCS1PrivateKey(block.Bytes)
}

// accessToken returns an OAuth token for FCM, refreshing it five minutes
// before it expires.
func (s *FCMSender) accessToken(ctx context.Context) (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.token != "" && s.now().Before(s.expires.Add(-5*time.Minute)) {
		return s.token, nil
	}
	tokenURL := s.cfg.TokenURL
	if tokenURL == "" {
		tokenURL = fcmTokenURL
	}
	now := s.now()
	header := b64([]byte(`{"alg":"RS256","typ":"JWT"}`))
	claims, err := json.Marshal(map[string]any{
		"iss":   s.account.ClientEmail,
		"scope": fcmScope,
		"aud":   tokenURL,
		"iat":   now.Unix(),
		"exp":   now.Add(time.Hour).Unix(),
	})
	if err != nil {
		return "", err
	}
	signing := header + "." + b64(claims)
	digest := sha256.Sum256([]byte(signing))
	sig, err := rsa.SignPKCS1v15(rand.Reader, s.key, crypto.SHA256, digest[:])
	if err != nil {
		return "", err
	}
	form := url.Values{
		"grant_type": {"urn:ietf:params:oauth:grant-type:jwt-bearer"},
		"assertion":  {signing + "." + b64(sig)},
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, tokenURL, strings.NewReader(form.Encode()))
	if err != nil {
		return "", err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	resp, err := s.client.Do(req)
	if err != nil {
		return "", err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	if resp.StatusCode != http.StatusOK {
		return "", fmt.Errorf("FCM token: HTTP %d", resp.StatusCode)
	}
	var token struct {
		AccessToken string `json:"access_token"`
		ExpiresIn   int    `json:"expires_in"`
	}
	if err = json.Unmarshal(body, &token); err != nil || token.AccessToken == "" {
		return "", errors.New("FCM token: no access_token in the answer")
	}
	s.token = token.AccessToken
	s.expires = now.Add(time.Duration(token.ExpiresIn) * time.Second)
	return s.token, nil
}

// Send wakes the device with registration |token| for |kind| (e.g.
// "send-tab"). The result's Status is FCM's HTTP status; Reason its error
// status ("UNREGISTERED", "INVALID_ARGUMENT", ...).
func (s *FCMSender) Send(ctx context.Context, token, kind string) (Result, error) {
	access, err := s.accessToken(ctx)
	if err != nil {
		return Result{}, err
	}
	message, err := json.Marshal(map[string]any{
		"message": map[string]any{
			"token":   token,
			"data":    map[string]string{"type": kind},
			"android": map[string]any{"priority": "high", "ttl": "86400s"},
		},
	})
	if err != nil {
		return Result{}, err
	}
	sendURL := s.cfg.SendURL
	if sendURL == "" {
		sendURL = fcmSendURL
	}
	if strings.Contains(sendURL, "%s") {
		sendURL = fmt.Sprintf(sendURL, url.PathEscape(s.account.ProjectID))
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, sendURL, bytes.NewReader(message))
	if err != nil {
		return Result{}, err
	}
	req.Header.Set("Authorization", "Bearer "+access)
	req.Header.Set("Content-Type", "application/json")
	resp, err := s.client.Do(req)
	if err != nil {
		return Result{}, err
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 64<<10))
	res := Result{Status: resp.StatusCode}
	if resp.StatusCode != http.StatusOK {
		var failure struct {
			Error struct {
				Status string `json:"status"`
			} `json:"error"`
		}
		if json.Unmarshal(body, &failure) == nil {
			res.Reason = failure.Error.Status
		}
	}
	return res, nil
}

// FCMInvalid says whether a send's result means the registration is gone.
func FCMInvalid(r Result) bool {
	return r.Status == http.StatusNotFound || r.Reason == "UNREGISTERED" ||
		(r.Status == http.StatusBadRequest && r.Reason == "INVALID_ARGUMENT")
}

// validFCMToken: FCM registration tokens are long and URL-safe.
func validFCMToken(t string) bool {
	if len(t) < 32 || len(t) > 4096 {
		return false
	}
	for _, c := range t {
		if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' ||
			c == ':' || c == '_' || c == '-') {
			return false
		}
	}
	return true
}
