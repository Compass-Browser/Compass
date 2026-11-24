package push

// The Compass push relay (server/cloudflare/relay/README.md), for a server
// without APNs or FCM keys of its own: devices' tokens are sealed by the
// relay into handles only it can open, kept here in their place, and
// pushes are asked of the relay, which sends Compass's two fixed pushes.
// Off unless COMPASS_SYNC_PUSH_RELAY is set, and only used while neither
// an APNs key nor an FCM account is configured.
//
// A handle is stored as a token whose environment is RelayAPNs or
// RelayFCM, and whose topic is the server's host the handle is bound to.

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"
)

const (
	RelayAPNs = "relay-apns"
	RelayFCM  = "relay-fcm"
)

// ErrRelayRefused is the relay refusing a registration (400 or 403).
var ErrRelayRefused = errors.New("the push relay refused the registration")

func isRelay(environment string) bool { return strings.HasPrefix(environment, "relay-") }

// RelayClient talks to the relay.
type RelayClient struct {
	url    string
	client *http.Client
}

// NewRelayClient takes the relay's base URL (https://push.compassbrowser.org).
func NewRelayClient(url string, client *http.Client) *RelayClient {
	if client == nil {
		client = &http.Client{Timeout: 15 * time.Second}
	}
	return &RelayClient{url: strings.TrimRight(url, "/"), client: client}
}

func (c *RelayClient) post(ctx context.Context, path string, body any) (int, map[string]any, error) {
	raw, err := json.Marshal(body)
	if err != nil {
		return 0, nil, err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.url+path, bytes.NewReader(raw))
	if err != nil {
		return 0, nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := c.client.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	var out map[string]any
	data, _ := io.ReadAll(io.LimitReader(resp.Body, 16<<10))
	_ = json.Unmarshal(data, &out)
	return resp.StatusCode, out, nil
}

// Seal has the relay seal a registration for |server| (this server's host).
func (c *RelayClient) Seal(ctx context.Context, server, platform, token, environment, topic string) (string, error) {
	body := map[string]string{"server": server, "platform": platform, "token": token, "topic": topic}
	if platform == "apns" {
		body["environment"] = environment
	}
	status, out, err := c.post(ctx, "/v1/seal", body)
	switch {
	case err != nil:
		return "", err
	case status == http.StatusBadRequest || status == http.StatusForbidden:
		return "", ErrRelayRefused
	case status != http.StatusOK:
		return "", fmt.Errorf("push relay: seal: HTTP %d", status)
	}
	handle, _ := out["handle"].(string)
	if handle == "" {
		return "", errors.New("push relay: no handle in the answer")
	}
	return handle, nil
}

// Push asks for |kind| ("send-tab" or "sync") to the device of |handle|.
// drop says the handle is no good any more (the device is gone, or the
// relay can't open it), so it should be forgotten.
func (c *RelayClient) Push(ctx context.Context, server, handle, kind string) (drop bool, err error) {
	status, out, err := c.post(ctx, "/v1/push", map[string]string{"server": server, "handle": handle, "type": kind})
	if err != nil {
		return false, err
	}
	code, _ := out["error"].(string)
	switch {
	case status == http.StatusOK:
		return false, nil
	case status == http.StatusGone || code == "bad_handle":
		return true, nil
	default:
		return false, fmt.Errorf("push relay: %s: HTTP %d %s", kind, status, code)
	}
}
