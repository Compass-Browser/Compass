package auth

import (
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
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

// authenticate parses the access token and verifies the signature and the
// timestamp is within +/- 1 day. The acccess token format is:
// base64_encode(timestamp_hex_encoded|signed_timestamp_hex_encoded|public_key_hex_encoded)
func authenticate(tkn string) (string, error) {
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

	// Compass: a private server serves only the chains it lists (README,
	// "Private server"). The ID is in the error so the admin can add it.
	if allowed := idList("COMPASS_SYNC_ALLOWED_CHAINS"); len(allowed) > 0 &&
		!slices.Contains(allowed, id) {
		return "", fmt.Errorf("chain not allowed: %s", id)
	}

	return token.PublicKeyHex, nil
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
	var token string
	// Extract token from the header.
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

	// Verify token
	clientID, err := authenticate(token)
	if err != nil {
		return "", fmt.Errorf("error authorizing: %w", err)
	}
	return clientID, nil
}
