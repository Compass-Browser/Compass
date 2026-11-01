// Package clientip finds the IP address of the client behind a request,
// for rate limits.
//
// By default that is the TCP peer: X-Forwarded-For is set by whoever sends
// the request, so trusting it would let a client pick its own rate-limit
// bucket (go-sync dropped chi's RealIP middleware for that reason). Behind
// a reverse proxy (Caddy in docker-compose.yml) every request comes from the
// proxy, so the proxy's addresses can be listed as trusted: for a request
// from a trusted peer, the client is the right-most X-Forwarded-For entry
// that isn't itself a trusted proxy. Caddy appends the address it got the
// connection from, so a client can only prepend entries, which are skipped.
package clientip

import (
	"fmt"
	"net"
	"net/http"
	"net/netip"
	"strings"
)

// Resolver maps requests to client IPs.
type Resolver struct {
	trusted []netip.Prefix
}

// ParseTrusted parses a comma-separated list of CIDRs or single addresses,
// e.g. "172.30.0.0/24, 10.0.0.1". Empty means no trusted proxies.
func ParseTrusted(s string) ([]netip.Prefix, error) {
	var out []netip.Prefix
	for _, part := range strings.Split(s, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		if strings.Contains(part, "/") {
			p, err := netip.ParsePrefix(part)
			if err != nil {
				return nil, fmt.Errorf("invalid trusted proxy %q: %w", part, err)
			}
			out = append(out, p.Masked())
			continue
		}
		a, err := netip.ParseAddr(part)
		if err != nil {
			return nil, fmt.Errorf("invalid trusted proxy %q: %w", part, err)
		}
		a = a.Unmap()
		out = append(out, netip.PrefixFrom(a, a.BitLen()))
	}
	return out, nil
}

// New returns a Resolver trusting X-Forwarded-For only from |trusted|.
func New(trusted []netip.Prefix) *Resolver {
	return &Resolver{trusted: trusted}
}

func (r *Resolver) isTrusted(a netip.Addr) bool {
	for _, p := range r.trusted {
		if p.Contains(a) {
			return true
		}
	}
	return false
}

func parseAddr(s string) (netip.Addr, bool) {
	s = strings.TrimSpace(s)
	if host, _, err := net.SplitHostPort(s); err == nil {
		s = host
	}
	a, err := netip.ParseAddr(strings.Trim(s, "[]"))
	if err != nil {
		return netip.Addr{}, false
	}
	return a.Unmap().WithZone(""), true
}

// IP returns the client's address for |req|.
func (r *Resolver) IP(req *http.Request) string {
	peer, ok := parseAddr(req.RemoteAddr)
	if !ok {
		return req.RemoteAddr
	}
	if !r.isTrusted(peer) {
		return peer.String()
	}
	var hops []string
	for _, line := range req.Header.Values("X-Forwarded-For") {
		hops = append(hops, strings.Split(line, ",")...)
	}
	client := peer
	for i := len(hops) - 1; i >= 0; i-- {
		a, ok := parseAddr(hops[i])
		if !ok {
			break // Garbage: stop at the last address we could trust.
		}
		client = a
		if !r.isTrusted(a) {
			break
		}
	}
	return client.String()
}
