package push

import (
	"context"
	"crypto"
	"crypto/rand"
	"crypto/rsa"
	"crypto/sha256"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
)

// fakeGoogle serves the OAuth token endpoint and FCM's send endpoint.
type fakeGoogle struct {
	key       *rsa.PublicKey
	tokens    atomic.Int32
	messages  []map[string]any
	sendState int // HTTP status for sends
	reason    string
}

func (f *fakeGoogle) handler(t *testing.T) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/token", func(w http.ResponseWriter, r *http.Request) {
		if err := r.ParseForm(); err != nil {
			t.Fatal(err)
		}
		if r.Form.Get("grant_type") != "urn:ietf:params:oauth:grant-type:jwt-bearer" {
			t.Errorf("grant_type = %q", r.Form.Get("grant_type"))
		}
		parts := strings.Split(r.Form.Get("assertion"), ".")
		if len(parts) != 3 {
			t.Fatalf("assertion isn't a JWT")
		}
		sig, _ := base64.RawURLEncoding.DecodeString(parts[2])
		digest := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
		if err := rsa.VerifyPKCS1v15(f.key, crypto.SHA256, digest[:], sig); err != nil {
			t.Errorf("bad signature: %v", err)
		}
		claims, _ := base64.RawURLEncoding.DecodeString(parts[1])
		if !strings.Contains(string(claims), "firebase.messaging") {
			t.Errorf("claims %s lack the FCM scope", claims)
		}
		f.tokens.Add(1)
		io.WriteString(w, `{"access_token":"tok","expires_in":3600}`)
	})
	mux.HandleFunc("/v1/projects/proj/messages:send", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer tok" {
			t.Errorf("Authorization = %q", r.Header.Get("Authorization"))
		}
		var m map[string]any
		json.NewDecoder(r.Body).Decode(&m)
		f.messages = append(f.messages, m)
		w.WriteHeader(f.sendState)
		if f.reason != "" {
			io.WriteString(w, `{"error":{"status":"`+f.reason+`"}}`)
		} else {
			io.WriteString(w, `{"name":"projects/proj/messages/1"}`)
		}
	})
	return mux
}

func newTestFCM(t *testing.T) (*FCMSender, *fakeGoogle) {
	t.Helper()
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatal(err)
	}
	der, _ := x509.MarshalPKCS8PrivateKey(key)
	pemKey := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der})
	fake := &fakeGoogle{key: &key.PublicKey, sendState: http.StatusOK}
	srv := httptest.NewServer(fake.handler(t))
	t.Cleanup(srv.Close)
	account, _ := json.Marshal(map[string]string{
		"project_id":   "proj",
		"client_email": "push@proj.iam.gserviceaccount.com",
		"private_key":  string(pemKey),
	})
	s, err := newFCMSender(FCMConfig{
		TokenURL: srv.URL + "/token",
		SendURL:  srv.URL + "/v1/projects/%s/messages:send",
	}, account)
	if err != nil {
		t.Fatal(err)
	}
	return s, fake
}

func TestFCMSendsAWakeUp(t *testing.T) {
	s, fake := newTestFCM(t)
	for range 2 {
		res, err := s.Send(context.Background(), strings.Repeat("a", 40)+":b-c_d", "send-tab")
		if err != nil || res.Status != http.StatusOK {
			t.Fatalf("Send = %+v, %v", res, err)
		}
	}
	if n := fake.tokens.Load(); n != 1 {
		t.Errorf("fetched %d OAuth tokens, want 1 (cached)", n)
	}
	message := fake.messages[0]["message"].(map[string]any)
	if message["data"].(map[string]any)["type"] != "send-tab" {
		t.Errorf("message = %v", message)
	}
	if message["android"].(map[string]any)["priority"] != "high" {
		t.Errorf("message = %v, want high priority", message)
	}
}

func TestFCMUnregisteredIsInvalid(t *testing.T) {
	s, fake := newTestFCM(t)
	fake.sendState = http.StatusNotFound
	fake.reason = "UNREGISTERED"
	res, err := s.Send(context.Background(), strings.Repeat("x", 40), "send-tab")
	if err != nil {
		t.Fatal(err)
	}
	if !FCMInvalid(res) {
		t.Errorf("FCMInvalid(%+v) = false", res)
	}
	if FCMInvalid(Result{Status: http.StatusServiceUnavailable, Reason: "UNAVAILABLE"}) {
		t.Error("a temporary failure counts as invalid")
	}
}

func TestValidFCMToken(t *testing.T) {
	good := "dGhpcyBpcyBhIHRva2Vu:APA91b" + strings.Repeat("Ab-_9", 20)
	if !validFCMToken(good) {
		t.Errorf("%q refused", good)
	}
	for _, bad := range []string{"short", strings.Repeat("a", 40) + " x", strings.Repeat("a", 40) + "/"} {
		if validFCMToken(bad) {
			t.Errorf("%q accepted", bad)
		}
	}
}
