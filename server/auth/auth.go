package auth

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"regexp"
	"slices"
	"strconv"
	"strings"
	"time"
)

const (
	// TokenMaxDuration specifies the lifetime for each access token.
	TokenMaxDuration int64  = 86400 * 1e3 // Milliseconds
	bearerPrefix     string = "Bearer "
	tokenRE          string = `^(?P<TimestampHex>[a-fA-F0-9]+)\|(?P<SignedTimestampHex>[a-fA-F0-9]+)\|(?P<PublicKeyHex>[a-fA-F0-9]+)$` //nolint:gosec // G101 false positive: regex pattern, not a credential
)

// Token represents the values we have in access tokens.
type Token struct {
	TimestampHex       string
	SignedTimestampHex string
	PublicKeyHex       string
}

func abs(n int64) int64 {
	if n < 0 {
		return -n
	}
	return n
}

// verifyToken parses the access token and verifies the signature and the
// timestamp is within +/- 1 day, and that the chain isn't blocked
// (BLOCKED_CLIENT_IDS). The acccess token format is:
// base64_encode(timestamp_hex_encoded|signed_timestamp_hex_encoded|public_key_hex_encoded)
func verifyToken(tkn string) (string, error) {
	base64DecodedBytes, err := base64.URLEncoding.DecodeString(tkn)
	if err != nil {
		return "", fmt.Errorf("error doing base64 decoding: %w", err)
	}

	re := regexp.MustCompile(tokenRE)
	m := re.FindStringSubmatch(string(base64DecodedBytes))
	if m == nil {
		return "", errors.New("invalid token format")
	}
	token := Token{TimestampHex: m[1], SignedTimestampHex: m[2], PublicKeyHex: m[3]}

	// Verify the signature.
	publicKey, err := hex.DecodeString(token.PublicKeyHex)
	if err != nil {
		return "", fmt.Errorf("error decoding hex string: %w", err)
	}
	timestampBytes, err := hex.DecodeString(token.TimestampHex)
	if err != nil {
		return "", fmt.Errorf("error decoding hex string: %w", err)
	}
	signedTimestamp, err := hex.DecodeString(token.SignedTimestampHex)
	if err != nil {
		return "", fmt.Errorf("error decoding hex string: %w", err)
	}
	if !ed25519.Verify(publicKey, timestampBytes, signedTimestamp) {
		return "", errors.New("signature verification failed")
	}

	var timestamp int64
	timestamp, err = strconv.ParseInt(string(timestampBytes), 10, 64)
	if err != nil {
		return "", fmt.Errorf("error parsing timestamp: %w", err)
	}

	// Verify that this token is within +/- 1 day.
	if abs(time.Now().UnixMilli()-timestamp) > TokenMaxDuration {
		return "", errors.New("token is expired")
	}

	id := strings.ToLower(token.PublicKeyHex)
	if slices.Contains(idList("BLOCKED_CLIENT_IDS"), id) {
		return "", errors.New("this client ID is blocked")
	}

	return token.PublicKeyHex, nil
}

// authenticate is verifyToken plus the private server's allowlist.
func authenticate(tkn string) (string, error) {
	id, err := verifyToken(tkn)
	if err != nil {
		return "", err
	}
	// Compass: a private server serves only the chains it lists (README,
	// "Private server"). The ID is in the error so the admin can add it.
	if !ChainListed(id) {
		return "", fmt.Errorf("chain not allowed: %s", strings.ToLower(id))
	}
	return id, nil
}

// ChainListed says whether chain |id| (any case) may use the whole server:
// true unless COMPASS_SYNC_ALLOWED_CHAINS is set and doesn't list it.
func ChainListed(id string) bool {
	allowed := idList("COMPASS_SYNC_ALLOWED_CHAINS")
	return len(allowed) == 0 || slices.Contains(allowed, strings.ToLower(id))
}

// idList reads a comma-separated list of chain IDs (hex public keys) from
// the environment variable |name|, lowercased, without blanks.
func idList(name string) []string {
	var ids []string
	for _, s := range strings.Split(os.Getenv(name), ",") {
		if s = strings.ToLower(strings.TrimSpace(s)); s != "" {
			ids = append(ids, s)
		}
	}
	return ids
}

// Authorize extracts the authorize token from the HTTP request and verify if
// the token is valid or not. It returns the clientID if the token is valid,
// otherwise, an empty string will be returned.
func Authorize(r *http.Request) (string, error) {
	token, err := bearer(r)
	if err != nil {
		return "", err
	}

	// Verify token
	clientID, err := authenticate(token)
	if err != nil {
		return "", fmt.Errorf("error authorizing: %w", err)
	}
	return clientID, nil
}

// bearer returns the request's "Authorization: Bearer" token.
func bearer(r *http.Request) (string, error) {
	var token string
	tokens, ok := r.Header["Authorization"]
	if ok && len(tokens) >= 1 {
		token = tokens[0]
		if !strings.HasPrefix(token, bearerPrefix) {
			return "", errors.New("not a valid token")
		}
		token = strings.TrimPrefix(token, bearerPrefix)
	}
	if token == "" {
		return "", errors.New("not a valid token")
	}
	return token, nil
}

// AuthenticateToken checks a sync token (the Authorization value without
// "Bearer ", as in X-Compass-Linked-Token) and returns its chain ID, as
// Authorize does.
func AuthenticateToken(token string) (string, error) {
	token = strings.TrimPrefix(strings.TrimSpace(token), bearerPrefix)
	if token == "" {
		return "", errors.New("not a valid token")
	}
	clientID, err := authenticate(token)
	if err != nil {
		return "", fmt.Errorf("error authorizing: %w", err)
	}
	return clientID, nil
}

// Compass: optional company sign-in (docs/design/sync.md, "Company sign-in
// (OIDC)"). When it's on, a valid token isn't enough: the chain must be
// bound to a signed-in user.

// ErrLoginRequired is returned by an AuthorizeSync whose gate refuses an
// unbound chain. Handlers answer it with WriteLoginRequired, never with
// DISABLED_BY_ADMIN (which makes Chromium stop sync and clear its data).
var ErrLoginRequired = errors.New("login_required")

// ErrGateUnavailable is returned by an AuthorizeSync whose gate couldn't
// tell (the database failed): handlers answer 503.
var ErrGateUnavailable = errors.New("login check unavailable")

// LoginHeader is set to "required" on a login_required answer.
const LoginHeader = "X-Compass-Login"

// LoginGate says whether chain |chainID| (lower-case) may sync: nil,
// ErrLoginRequired, or another error if it couldn't tell.
type LoginGate func(ctx context.Context, chainID string) error

// AuthorizeSync is Authorize followed by |gate|. With a nil gate it is
// Authorize itself (sign-in off: nothing changes).
func AuthorizeSync(gate LoginGate) func(*http.Request) (string, error) {
	if gate == nil {
		return Authorize
	}
	return func(r *http.Request) (string, error) {
		id, err := Authorize(r)
		if err != nil {
			return "", err
		}
		if err = gate(r.Context(), strings.ToLower(id)); err != nil {
			if errors.Is(err, ErrLoginRequired) {
				return "", ErrLoginRequired
			}
			return "", fmt.Errorf("%w: %w", ErrGateUnavailable, err)
		}
		return id, nil
	}
}

// WriteLoginRequired answers 401 {"error":"login_required"}.
func WriteLoginRequired(w http.ResponseWriter) {
	w.Header().Set(LoginHeader, "required")
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusUnauthorized)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": "login_required"})
}

// WriteGateError answers an AuthorizeSync error that came from the gate
// (401 login_required, or 503), or a RejectRemovedDevices refusal (403
// device_removed), and returns true; false for any other error,
// which the caller answers as before.
func WriteGateError(w http.ResponseWriter, err error) bool {
	switch {
	case errors.Is(err, ErrLoginRequired):
		WriteLoginRequired(w)
		return true
	case errors.Is(err, ErrDeviceRemoved):
		WriteDeviceRemoved(w)
		return true
	case errors.Is(err, ErrGateUnavailable):
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(http.StatusServiceUnavailable)
		_ = json.NewEncoder(w).Encode(map[string]string{"error": "login check unavailable"})
		return true
	}
	return false
}

// Compass: removed devices (docs/design/sync.md, "Identity"). A device
// removed from its chain is refused by its cache GUID, which every
// per-device request carries as the client_id query parameter.

// ErrDeviceRemoved is returned by a RejectRemovedDevices authorize for a
// request whose client_id was removed from the chain. Handlers answer it
// with WriteDeviceRemoved (WriteGateError does).
var ErrDeviceRemoved = errors.New("device_removed")

// DeviceHeader is set to "removed" on a device_removed answer.
const DeviceHeader = "X-Compass-Device"

// DeviceRemovedCheck says whether |cacheGUID| was removed from chain
// |chainID| (as the token gave it).
type DeviceRemovedCheck func(ctx context.Context, chainID, cacheGUID string) (bool, error)

// RejectRemovedDevices is |authorize| that also refuses a request whose
// client_id query parameter is a device removed from the chain
// (ErrDeviceRemoved). Requests without a client_id are unaffected.
func RejectRemovedDevices(authorize func(*http.Request) (string, error),
	removed DeviceRemovedCheck) func(*http.Request) (string, error) {
	return func(r *http.Request) (string, error) {
		id, err := authorize(r)
		if id == "" || err != nil {
			return id, err
		}
		guid := r.URL.Query().Get("client_id")
		if guid == "" {
			return id, nil
		}
		gone, err := removed(r.Context(), id, guid)
		if err != nil {
			return "", fmt.Errorf("%w: %w", ErrGateUnavailable, err)
		}
		if gone {
			return "", ErrDeviceRemoved
		}
		return id, nil
	}
}

// WriteDeviceRemoved answers 403 {"error":"device_removed"}.
func WriteDeviceRemoved(w http.ResponseWriter) {
	w.Header().Set(DeviceHeader, "removed")
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusForbidden)
	_ = json.NewEncoder(w).Encode(map[string]string{"error": "device_removed"})
}

// Compass: rooms (docs/design/sync.md, "Rooms"). On a private server
// (COMPASS_SYNC_ALLOWED_CHAINS) a chain it doesn't list may still use the
// room routes of rooms it's a member of, or holds an invite to; the rooms
// handler checks that, given |listed|.

// RoomAuthorize is AuthorizeRooms' result: the chain ID as the token sent
// it, and whether the chain may use the whole server (ChainListed).
type RoomAuthorize func(r *http.Request) (chainID string, listed bool, err error)

// AuthorizeRooms checks a sync token as Authorize does, except that a
// chain the private server doesn't list isn't refused (listed is false),
// then |gate| (company sign-in; nil: none) and, if the request has a
// client_id, |removed| (nil: no check), as AuthorizeSync and
// RejectRemovedDevices. Errors from the gate and the removed check are
// answered by WriteGateError.
func AuthorizeRooms(gate LoginGate, removed DeviceRemovedCheck) RoomAuthorize {
	return func(r *http.Request) (string, bool, error) {
		token, err := bearer(r)
		if err != nil {
			return "", false, err
		}
		id, err := verifyToken(token)
		if err != nil {
			return "", false, fmt.Errorf("error authorizing: %w", err)
		}
		if gate != nil {
			if err = gate(r.Context(), strings.ToLower(id)); err != nil {
				if errors.Is(err, ErrLoginRequired) {
					return "", false, ErrLoginRequired
				}
				return "", false, fmt.Errorf("%w: %w", ErrGateUnavailable, err)
			}
		}
		if guid := r.URL.Query().Get("client_id"); guid != "" && removed != nil {
			gone, err := removed(r.Context(), id, guid)
			if err != nil {
				return "", false, fmt.Errorf("%w: %w", ErrGateUnavailable, err)
			}
			if gone {
				return "", false, ErrDeviceRemoved
			}
		}
		return id, ChainListed(id), nil
	}
}
