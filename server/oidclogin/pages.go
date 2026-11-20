package oidclogin

import (
	"bytes"
	"html/template"
	"net/http"

	"github.com/rs/zerolog/log"
)

type consentPage struct {
	Provider, Email, Name, Device, Code, ID, CSRF string
}

type donePage struct {
	Provider string
	Allowed  bool
}

type errorPage struct {
	Message string
}

// The pages load nothing: styles are inline, there are no scripts.
var pages = template.Must(template.New("base").Parse(`
{{define "head"}}<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="referrer" content="no-referrer">
<title>{{.}}</title>
<style>
:root { color-scheme: light dark; --bg: #f6f6f4; --card: #fff; --fg: #1d1d1f; --muted: #6b6b70;
  --line: #e2e2e0; --accent: #2f5bea; --accent-fg: #fff; }
@media (prefers-color-scheme: dark) { :root { --bg: #18181a; --card: #232326; --fg: #f2f2f3;
  --muted: #a0a0a6; --line: #38383c; --accent: #6d8dff; --accent-fg: #0d0d10; } }
* { box-sizing: border-box; }
body { margin: 0; min-height: 100vh; display: flex; align-items: center; justify-content: center;
  background: var(--bg); color: var(--fg); padding: 16px;
  font: 16px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
main { width: 100%; max-width: 420px; background: var(--card); border: 1px solid var(--line);
  border-radius: 14px; padding: 28px; }
h1 { font-size: 20px; margin: 0 0 12px; }
p { margin: 0 0 14px; }
.muted { color: var(--muted); font-size: 14px; }
dl { margin: 0 0 18px; display: grid; grid-template-columns: auto 1fr; gap: 6px 14px; font-size: 15px; }
dt { color: var(--muted); }
dd { margin: 0; overflow-wrap: anywhere; }
.code { font: 600 30px/1 ui-monospace, SFMono-Regular, Menlo, monospace; letter-spacing: 3px;
  text-align: center; padding: 14px; border: 1px dashed var(--line); border-radius: 10px; margin: 0 0 8px; }
.actions { display: flex; gap: 10px; margin-top: 20px; }
.actions form { flex: 1; margin: 0; }
button { width: 100%; font: inherit; font-weight: 600; padding: 10px; border-radius: 9px;
  border: 1px solid var(--line); background: transparent; color: var(--fg); cursor: pointer; }
button.primary { background: var(--accent); border-color: var(--accent); color: var(--accent-fg); }
</style></head><body><main>{{end}}
{{define "foot"}}</main></body></html>{{end}}

{{define "consent"}}{{template "head" "Allow Compass to sync"}}
<h1>Allow this device to sync?</h1>
<p class="muted">You signed in with {{.Provider}}.</p>
<dl>
<dt>Account</dt><dd>{{if .Name}}{{.Name}} · {{end}}{{.Email}}</dd>
<dt>Device</dt><dd>{{.Device}}</dd>
</dl>
<div class="code">{{.Code}}</div>
<p class="muted">Check that Compass shows the same code. If it doesn't, or you didn't start this, choose Cancel.</p>
<div class="actions">
<form method="post" action="/v2/compass/login/{{.ID}}/deny"><input type="hidden" name="csrf" value="{{.CSRF}}"><button type="submit">Cancel</button></form>
<form method="post" action="/v2/compass/login/{{.ID}}/approve"><input type="hidden" name="csrf" value="{{.CSRF}}"><button type="submit" class="primary">Allow</button></form>
</div>
{{template "foot"}}{{end}}

{{define "done"}}{{template "head" "Compass sync"}}
{{if .Allowed}}<h1>You're signed in</h1>
<p>You can return to Compass. You can close this tab.</p>
{{else}}<h1>Sign-in cancelled</h1>
<p>Compass wasn't signed in. You can close this tab.</p>{{end}}
{{template "foot"}}{{end}}

{{define "error"}}{{template "head" "Sign-in failed"}}
<h1>Sign-in failed</h1>
<p>{{.Message}}</p>
<p class="muted">You can close this tab.</p>
{{template "foot"}}{{end}}
`))

// securityHeaders lock a page down: no scripts, no external loads, no
// frames. |toApp| lets its forms end in a redirect to compass:// (browsers
// check form-action on redirects too).
func securityHeaders(w http.ResponseWriter, toApp bool) {
	formAction := "form-action 'self'"
	if toApp {
		formAction += " compass:"
	}
	h := w.Header()
	h.Set("Content-Security-Policy",
		"default-src 'none'; style-src 'unsafe-inline'; "+formAction+"; frame-ancestors 'none'; base-uri 'none'")
	h.Set("X-Frame-Options", "DENY")
	h.Set("X-Content-Type-Options", "nosniff")
	h.Set("Referrer-Policy", "no-referrer")
	h.Set("Cache-Control", "no-store")
}

func (s *Service) render(w http.ResponseWriter, status int, name string, data any, toApp bool) {
	var buf bytes.Buffer
	if err := pages.ExecuteTemplate(&buf, name, data); err != nil {
		log.Error().Err(err).Str("page", name).Msg("Sign-in: rendering failed")
		http.Error(w, "internal error", http.StatusInternalServerError)
		return
	}
	securityHeaders(w, toApp)
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(status)
	_, _ = w.Write(buf.Bytes())
}

func (s *Service) errorPage(w http.ResponseWriter, status int, msg string) {
	s.render(w, status, "error", errorPage{Message: msg}, false)
}
