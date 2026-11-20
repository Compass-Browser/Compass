package authtest

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"strconv"
	"strings"
)

// GenerateToken generates token from a given timestamp for tests to use.
func GenerateToken(timestamp int64) (string, string, string, error) {
	timestampBytes := []byte(strconv.FormatInt(timestamp, 10))
	publicKey, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		return "", "", "", fmt.Errorf("generate key error: %w", err)
	}

	signedTimestampBytes := ed25519.Sign(privateKey, timestampBytes)
	publicKeyHex := hex.EncodeToString(publicKey)

	token := hex.EncodeToString(timestampBytes) + "|" +
		hex.EncodeToString(signedTimestampBytes) + "|" + publicKeyHex

	return base64.URLEncoding.EncodeToString([]byte(token)), token, publicKeyHex, nil
}

// TokenFor makes a sync token for |key| at |timestamp| (Unix ms), with
// upper-case hex as Chromium sends it, and returns it with the chain ID
// (lower-case hex public key).
func TokenFor(key ed25519.PrivateKey, timestamp int64) (token, chainID string) {
	ts := []byte(strconv.FormatInt(timestamp, 10))
	pub := hex.EncodeToString(key.Public().(ed25519.PublicKey))
	plain := strings.ToUpper(hex.EncodeToString(ts)) + "|" +
		strings.ToUpper(hex.EncodeToString(ed25519.Sign(key, ts))) + "|" + strings.ToUpper(pub)
	return base64.URLEncoding.EncodeToString([]byte(plain)), pub
}
