package clientip_test

import (
	"net/http/httptest"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"

	"github.com/brave/go-sync/clientip"
)

func ip(t *testing.T, trusted, remote string, xff ...string) string {
	t.Helper()
	prefixes, err := clientip.ParseTrusted(trusted)
	require.NoError(t, err)
	req := httptest.NewRequest("GET", "/", nil)
	req.RemoteAddr = remote
	for _, v := range xff {
		req.Header.Add("X-Forwarded-For", v)
	}
	return clientip.New(prefixes).IP(req)
}

func TestNoTrustedProxyIgnoresForwardedFor(t *testing.T) {
	assert.Equal(t, "203.0.113.7", ip(t, "", "203.0.113.7:5555", "1.2.3.4"))
	assert.Equal(t, "2001:db8::1", ip(t, "", "[2001:db8::1]:443", "1.2.3.4"))
}

func TestUntrustedPeerCannotSpoof(t *testing.T) {
	assert.Equal(t, "203.0.113.7",
		ip(t, "172.30.0.0/24", "203.0.113.7:5555", "1.2.3.4"))
}

func TestTrustedProxy(t *testing.T) {
	// Caddy appends the real client; a forged entry in front is skipped.
	assert.Equal(t, "198.51.100.9",
		ip(t, "172.30.0.0/24", "172.30.0.2:4000", "6.6.6.6, 198.51.100.9"))
	// Several header lines count as one list.
	assert.Equal(t, "198.51.100.9",
		ip(t, "172.30.0.0/24", "172.30.0.2:4000", "6.6.6.6", "198.51.100.9"))
	// A chain of trusted proxies.
	assert.Equal(t, "198.51.100.9",
		ip(t, "172.30.0.0/24,10.0.0.1", "172.30.0.2:4000", "198.51.100.9, 10.0.0.1"))
	// No header: the proxy itself.
	assert.Equal(t, "172.30.0.2", ip(t, "172.30.0.0/24", "172.30.0.2:4000"))
	// Garbage stops the walk at the last good hop.
	assert.Equal(t, "172.30.0.2", ip(t, "172.30.0.0/24", "172.30.0.2:4000", "not-an-ip"))
	// IPv4-mapped IPv6 peers match IPv4 prefixes.
	assert.Equal(t, "198.51.100.9",
		ip(t, "172.30.0.0/24", "[::ffff:172.30.0.2]:4000", "198.51.100.9"))
}

func TestParseTrustedErrors(t *testing.T) {
	_, err := clientip.ParseTrusted("nope")
	require.Error(t, err)
	_, err = clientip.ParseTrusted("10.0.0.0/99")
	require.Error(t, err)
	p, err := clientip.ParseTrusted(" , ")
	require.NoError(t, err)
	assert.Empty(t, p)
}
