// Package push registers devices' APNs tokens per chain and wakes a chain's
// iPhones when another device sends them a tab (docs/design/send-tab.md,
// "Delivery").
//
//	POST   /?client_id=<cache GUID>  (sync token)  204
//	       {"token":"<hex>","environment":"sandbox"|"production","topic":"<bundle ID>"}
//	DELETE /?client_id=<cache GUID>  (sync token)  204
//
// When a commit adds a SEND_TAB_TO_SELF entity, every registered token of
// the chain except the committer's gets an alert push. The entry itself is
// encrypted, so the server can't tell which device it's for; the phone's
// notification service extension fetches and decrypts it.
package push

import (
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/rand"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"math/big"
	"net/http"
	"os"
	"strconv"
	"sync"
	"time"
)

// APNs hosts by registration environment.
const (
	ProductionHost = "https://api.push.apple.com"
	SandboxHost    = "https://api.sandbox.push.apple.com"
)

// APNsConfig is the provider key (Certificates, Identifiers & Profiles →
// Keys, "Apple Push Notifications service").
type APNsConfig struct {
	KeyPath string // The .p8 file.
	KeyID   string
	TeamID  string
	// Host, if set, replaces both hosts (tests).
	Host string
	// HTTPClient, if set, replaces the default HTTP/2 client (tests).
	HTTPClient *http.Client
}

// Enabled says whether a key is configured.
func (c APNsConfig) Enabled() bool { return c.KeyPath != "" }

// jwtLifetime: Apple refuses tokens older than an hour and too-frequent
// refreshes (more than once per 20 minutes).
const jwtLifetime = 40 * time.Minute

// Sender sends APNs pushes with token-based auth.
type Sender struct {
	cfg    APNsConfig
	key    *ecdsa.PrivateKey
	client *http.Client
	now    func() time.Time

	mu       sync.Mutex
	jwt      string
	jwtIssue time.Time
}

// NewSender loads the key in |cfg|.
func NewSender(cfg APNsConfig) (*Sender, error) {
	if cfg.KeyID == "" || cfg.TeamID == "" {
		return nil, errors.New("APNs key ID and team ID are required with a key")
	}
	raw, err := os.ReadFile(cfg.KeyPath)
	if err != nil {
		return nil, fmt.Errorf("error reading the APNs key: %w", err)
	}
	key, err := ParseKey(raw)
	if err != nil {
		return nil, err
	}
	client := cfg.HTTPClient
	if client == nil {
		client = &http.Client{
			Timeout:   15 * time.Second,
			Transport: &http.Transport{ForceAttemptHTTP2: true, MaxIdleConnsPerHost: 4},
		}
	}
	return &Sender{cfg: cfg, key: key, client: client, now: time.Now}, nil
}

// ParseKey reads a .p8 (PKCS#8 PEM, P-256) key.
func ParseKey(raw []byte) (*ecdsa.PrivateKey, error) {
	block, _ := pem.Decode(raw)
	if block == nil {
		return nil, errors.New("the APNs key is not PEM")
	}
	parsed, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, fmt.Errorf("error parsing the APNs key: %w", err)
	}
	key, ok := parsed.(*ecdsa.PrivateKey)
	if !ok || key.Curve.Params().BitSize != 256 {
		return nil, errors.New("the APNs key is not a P-256 key")
	}
	return key, nil
}

func b64(b []byte) string { return base64.RawURLEncoding.EncodeToString(b) }

// token returns the provider JWT (ES256), refreshed every jwtLifetime.
func (s *Sender) token() (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	if s.jwt != "" && now.Sub(s.jwtIssue) < jwtLifetime {
		return s.jwt, nil
	}
	header, _ := json.Marshal(map[string]string{"alg": "ES256", "kid": s.cfg.KeyID})
	claims, _ := json.Marshal(map[string]any{"iss": s.cfg.TeamID, "iat": now.Unix()})
	input := b64(header) + "." + b64(claims)
	digest := sha256.Sum256([]byte(input))
	r, sv, err := ecdsa.Sign(rand.Reader, s.key, digest[:])
	if err != nil {
		return "", fmt.Errorf("error signing the APNs token: %w", err)
	}
	sig := make([]byte, 64)
	r.FillBytes(sig[:32])
	sv.FillBytes(sig[32:])
	s.jwt = input + "." + b64(sig)
	s.jwtIssue = now
	return s.jwt, nil
}

// VerifyToken checks a provider JWT against |pub| (tests).
func VerifyToken(jwt string, pub *ecdsa.PublicKey) bool {
	var dot1, dot2 int = -1, -1
	for i := range len(jwt) {
		if jwt[i] == '.' {
			if dot1 < 0 {
				dot1 = i
			} else {
				dot2 = i
				break
			}
		}
	}
	if dot1 < 0 || dot2 < 0 {
		return false
	}
	sig, err := base64.RawURLEncoding.DecodeString(jwt[dot2+1:])
	if err != nil || len(sig) != 64 {
		return false
	}
	digest := sha256.Sum256([]byte(jwt[:dot2]))
	return ecdsa.Verify(pub, digest[:], new(big.Int).SetBytes(sig[:32]), new(big.Int).SetBytes(sig[32:]))
}

// Result is APNs' answer to one push.
type Result struct {
	Status int
	Reason string
}

// Invalid says whether APNs refused the token for good, so it should be
// dropped: 410 (unregistered), or 400 BadDeviceToken / DeviceTokenNotForTopic.
func (r Result) Invalid() bool {
	return r.Status == http.StatusGone ||
		(r.Status == http.StatusBadRequest &&
			(r.Reason == "BadDeviceToken" || r.Reason == "DeviceTokenNotForTopic"))
}

// Send pushes an alert |payload| to |token| on |topic|.
func (s *Sender) Send(ctx context.Context, environment, topic, token string, payload []byte) (Result, error) {
	host := ProductionHost
	if environment == "sandbox" {
		host = SandboxHost
	}
	if s.cfg.Host != "" {
		host = s.cfg.Host
	}
	jwt, err := s.token()
	if err != nil {
		return Result{}, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, host+"/3/device/"+token, bytes.NewReader(payload))
	if err != nil {
		return Result{}, err
	}
	req.Header.Set("Authorization", "bearer "+jwt)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("apns-push-type", "alert")
	req.Header.Set("apns-priority", "10")
	req.Header.Set("apns-topic", topic)
	// A sent tab older than a day isn't worth a banner; the app shows it.
	req.Header.Set("apns-expiration", strconv.FormatInt(s.now().Add(24*time.Hour).Unix(), 10))
	resp, err := s.client.Do(req)
	if err != nil {
		return Result{}, fmt.Errorf("error sending the push: %w", err)
	}
	defer resp.Body.Close()
	res := Result{Status: resp.StatusCode}
	if resp.StatusCode != http.StatusOK {
		var body struct {
			Reason string `json:"reason"`
		}
		raw, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		_ = json.Unmarshal(raw, &body)
		res.Reason = body.Reason
	}
	return res, nil
}
