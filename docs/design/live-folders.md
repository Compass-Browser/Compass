# Live folders

Status: plugin contract, SDK, built-in plugins and the Node reference runtime
are done (`plugins/`, `tools/live_folders/`), and so is the sidebar model
and its file format. The desktop runtime and sidebar UI
(`browser/live_folders/`, `browser/ui/live_folders/`) and sync (the
folder's source in `CompassNode.live_source`) are being built and tested;
installing plugins, Android and iOS follow. See "Plan" and "Order".

## Goal

Arc's live folders: a pinned folder whose entries come from a source and
refresh on their own, such as the latest posts of a feed, the pull requests
waiting for your review or the next meetings with their links. Sources are
**plugins written in JavaScript**, so one plugin runs everywhere Compass
runs: desktop and Android (Chromium, in a Worker) and iOS (JavaScriptCore).

Decisions:

- **Each device runs the plugin.** Only the folder's definition (plugin,
  config) syncs; the entries are generated and kept on each device and never
  synced. No write conflicts between devices, no sync churn.
- **Built-in plugins plus local install** from a URL or a file. No public
  catalog yet.
- **Auth:** a plugin can declare secrets (tokens, private URLs), kept in the
  platform's secure storage and never synced, and can ask to send the
  profile's cookies to declared hosts, which the user grants per folder.

## Plugins

A plugin is a folder (or a `.zip` of one):

```
manifest.json
main.js          the entry script
icon.svg         optional, 16×16, uses currentColor
```

### Manifest

```json
{
  "id": "compass.github",
  "name": "GitHub",
  "description": "Pull requests and issues that need you.",
  "version": "1.0.0",
  "api": 1,
  "entry": "main.js",
  "icon": "icon.svg",
  "hosts": ["api.github.com"],
  "cookies": [],
  "secrets": [{"key": "token", "label": "GitHub token", "help": "…", "optional": false}],
  "config": [
    {"key": "show", "type": "select", "label": "Show", "default": "review-requested",
     "options": [{"value": "review-requested", "label": "Review requests"}, "authored"]},
    {"key": "limit", "type": "number", "label": "Items", "default": 20, "min": 1, "max": 50}
  ],
  "refresh": {"default": 600, "min": 300}
}
```

| Key | Rules |
|---|---|
| `id` | Reverse-DNS style (`com.example.feed`), at most 100 characters. `compass.*` is for built-ins. |
| `version` | `1`, `1.2`, `1.2.3` or `1.2.3.4`. |
| `api` | `1`. A runtime refuses API versions it doesn't know. |
| `entry` | A `.js` file in the plugin folder; defaults to `main.js`. |
| `hosts` | Hosts `compass.fetch()` may reach: `example.com`, `*.example.com` (the domain and its subdomains), or `{config:key}` / `{secret:key}`, which allow the host of that URL field or secret. |
| `cookies` | Hosts in `hosts` that may receive the profile's cookies, once the user grants it for the folder. Same syntax. |
| `secrets` | Values the user enters once per device, stored encrypted, never synced: `{key, label, help?, optional?}`. |
| `config` | Fields of the folder's settings form, synced with the folder: `{key, type, label, default?, optional?, placeholder?}`; `type` is `string`, `url`, `number` (with `min`, `max`), `bool` or `select` (with `options`: strings or `{value, label}`). |
| `refresh` | Seconds between refreshes: `default` and `min`. Neither goes below 300. |

The rules are implemented in `plugins/runtime/manifest.js`, which is the
reference: native runtimes port it and are checked against the same cases.

### Entry script

The entry is a **classic script**, not an ES module, and defines one global
function:

```js
async function fetchItems(config, compass) {
  const res = await compass.fetch('https://api.example.com/items', {
    headers: {authorization: 'Bearer ' + compass.secret('token')},
  });
  if (!res.ok) throw new Error('The API answered HTTP ' + res.status);
  const data = await res.json();
  return {items: data.map((d) => ({id: d.id, title: d.name, url: d.link}))};
}
```

Classic scripts load the same way in a Worker, a `JSContext` and a Node vm
context, with no module loader or bundler. A plugin that needs several files
concatenates them.

`config` is the folder's config, validated against the manifest with
defaults filled in (`validateConfig`). Throwing fails the refresh; the
error's message is shown on the folder, so write it for the user.

### The `compass` object

| Member | |
|---|---|
| `fetch(url, {method, headers, body, credentials})` | `GET`, `HEAD` or `POST` to an allowed host. `credentials: "include"` sends cookies if the host is in `cookies` and the user granted it. Resolves to `{status, ok, url, headers, text(), json()}` (header names lower case, `url` after redirects). Not a full `fetch`: the body is always text. |
| `secret(key)` | A declared secret, or `null`. |
| `now()` | Epoch ms for this refresh. Use it rather than `Date.now()`, so tests are repeatable. |
| `locale`, `timeZone` | The user's, for formatting. |
| `log(...args)` | Shown in the developer view of the folder. |
| `text` | `decodeEntities`, `stripTags`, `collapse`, `parseAttributes`, `parseDate` (ISO 8601 and RFC 822 → ms). |
| `xml` | `parse(text)` → elements with `child`, `childText`, `elements`, `find`, `findAll`, `attr`, `text`. Lenient; never throws. |
| `html` | `links(page, pageUrl)` → `[{url, text, title}]`, `title(page)`. Pattern based, not a DOM. |
| `ics` | `parse(text)`, `occurrences(calendar, {from, to, timeZone})`: RRULE, EXDATE, RDATE, RECURRENCE-ID, TZID via `Intl`, Windows zone names. |

There is no DOM and no `DOMParser` (Workers and JavaScriptCore have none),
no `eval` or `new Function`, no timers beyond `setTimeout`, and nothing from
Node. The globals are ECMAScript's plus `URL`, `URLSearchParams`,
`TextEncoder`, `TextDecoder`, `setTimeout` and `clearTimeout`.

### Result

```js
{
  items: [{id, title, url, subtitle?, icon?, time?, badge?}],
  title?: "Suggested folder name",
  nextRefresh?: 600,     // seconds; clamped to [refresh.min, 1 day]
}
```

- `id`: stable for the same entry across refreshes (the runtime matches
  entries by it). Numbers become strings. Duplicates are dropped.
- `url`: absolute `http(s)`. `title`: required, whitespace collapsed,
  at most 300 characters. `badge`: at most 12 characters ("Draft", "Now").
- `time`: epoch ms or a date string. `icon`: an `http(s)` image URL.
- At most 50 items; the rest are dropped.
- `title` is used as the folder's name when the user creates it without one.

Invalid entries are dropped with a warning; only a result that isn't
`{items: [...]}` fails the refresh. Reference: `plugins/runtime/result.js`.

## Runtime

Each refresh runs in a fresh context that loads, in order:
`plugins/sdk/text.js`, `xml.js`, `html.js`, `ics.js`,
`plugins/runtime/prelude.js` and the plugin's entry. Before that the host
sets `globalThis.__compassHost` (its primitives: `fetch`, `log`, `env`); the
prelude captures it, removes the global and defines `compass` and the
entry point `__compassRun(config)`. The host normalizes the returned value
outside the plugin's context.

Nothing inside the context is trusted: the host enforces every rule.

| Limit | Value | Error code |
|---|---|---|
| Wall clock per refresh | 30 s | `timeout` |
| Fetches per refresh | 10 | `too_many_fetches` |
| Response size | 2 MB | `response_too_large` |
| Redirects | 5, each hop checked against `hosts` | `host_not_allowed` |
| Hosts | `hosts` after placeholders, http(s), no `user:pass@` | `host_not_allowed` |
| Request headers | `cookie`, `host`, `origin`, `referer`, `content-length`, `sec-*`, hop-by-hop: dropped | |

Other codes: `bad_manifest`, `bad_config`, `missing_secret`, `bad_plugin`
(no `fetchItems`), `bad_result`, `bad_request`, `network`, and
`plugin_error` for anything the plugin throws. Error codes are the
contract; messages may differ between runtimes.

Response bodies are decoded with the `charset` of `Content-Type`, UTF-8
otherwise.

## Built-in plugins

`plugins/builtin/`:

| Plugin | Source | Auth |
|---|---|---|
| `compass.rss` | RSS 2.0, RSS 1.0, Atom, JSON Feed | none |
| `compass.github` | Search API: review requests, my PRs, assigned, mentions, any query, optionally in one repo or owner | token (api.github.com doesn't accept the github.com session) |
| `compass.web-list` | Links on a page matching text or a `/regex/`, or a JSON list with field paths | optional cookies for that site |
| `compass.calendar` | Private iCal address (Google, Outlook, iCloud) → events in the next N hours, opening their Meet/Zoom/Teams/Webex/… link | the address is a secret |

## Testing

```
node --test plugins/sdk/tests/                 # SDK and manifest unit tests
node tools/live_folders/run.mjs test           # fixture and conformance cases
node tools/live_folders/run.mjs test --update  # rewrite expected.json files
node tools/live_folders/run.mjs run plugins/builtin/rss --config url=https://…
node tools/live_folders/run.mjs check path/to/plugin
```

`tools/live_folders/runtime.mjs` is the reference runtime: a Node vm
context with the same scripts, primitives and limits.

A case is a folder with `case.json` and `expected.json`:

- `plugins/builtin/<plugin>/tests/<case>/` runs that plugin.
- `plugins/conformance/<case>/` runs `plugin/` in the case folder; these
  cover the runtime rules (allowlist, limits, cookies, result rules).

`case.json` holds `config`, `secrets`, `grantCookies`, `env` (`now`,
`locale`, `timeZone`), `limits` and `responses`: recorded responses keyed
by URL (or `"POST <url>"`), each `{status, headers, file | body | json,
rejectHeaders}`. `expected.json` is the outcome: `{ok, result | error:
{code}, fetches: [{method, url, cookies}]}`.

Native runtimes run the same cases: the desktop runtime in a browser test,
the iOS one in `CompassKitTests`.

## Plan

### Data model (desktop and Android, shared C++)

- `SidebarNode` (`browser/sidebar/sidebar_node.h`): a folder gets an
  optional `LiveSource {plugin_id, config, plugin_url, plugin_sha256}`; a
  tab gets `live_item_key` (non-empty for generated entries) and `unseen`.
- `SpaceModel`: `AddLiveFolder`, `SetLiveSource`, `ConvertToPlainFolder`
  (entries become ordinary pinned tabs and start syncing) and
  `ReplaceLiveItems(folder, items)`, which diffs by key: matching entries
  are updated in place, new ones added in plugin order, vanished ones
  removed unless their tab is open (then they stay until closed). It fires
  the usual observer events, so UI and persistence need no new plumbing.
- `CanContain`: live folders only in the pinned section (clear of
  `CloseIfEmptyTodayFolder`); a live folder holds only its generated tabs.
  Dragging an entry out clears its key: it becomes an ordinary pinned tab.
- `sidebar_serializer.cc`: additive keys, version stays 2. Folder:
  `"live": {"plugin", "config", "source", "sha256", "refreshed"}`; tab:
  `"live_item"`, `"unseen"`. Entries are saved as a cache so the folder
  shows at once after a restart. An older build drops the keys and keeps a
  plain folder.

### Sync

- `CompassNode` field 15: `CompassLiveSource live_source {plugin_id,
  config_json, plugin_url, plugin_sha256}`. The node keeps `type = FOLDER`, so older
  clients show a plain, empty folder, and their mirrors keep field 15.
- The bridge never uploads nodes with a `live_item_key`, and applying
  remote changes doesn't delete a live folder's local entries for being
  absent remotely. Deleting the folder remotely still removes them (they
  aren't rescued to Pinned as other children of a deleted folder are).
- A folder made normal on one device brings that device's entries, as
  tabs; the other devices drop their own generated entries rather than
  upload them as copies. A remote config change updates the folder.
- Secrets and cookie grants are per device. The plugin's URL and hash sync,
  so a device without the plugin offers to install it.
- The server needs nothing: it stores specifics as opaque bytes.

### Desktop and Android runtime (`browser/live_folders/`)

- `LiveFolderService`, a per-profile `KeyedService`: plugin registry,
  scheduling, applying results with `ReplaceLiveItems`.
- A hidden `WebContents` per profile on `chrome-untrusted://live-folders/`
  (strict CSP, no network of its own) runs each refresh in its own
  dedicated Worker and relays the host primitives to C++ over
  `live_folders.mojom`.
- `compass.fetch` is `network::SimpleURLLoader` on the profile's
  `URLLoaderFactory`, with the allowlist checked on every redirect and
  cookies only for granted cookie hosts.
- Secrets are encrypted with `OSCryptAsync` in
  `<profile>/Compass Live Folders.json`, with grants, installed plugins and
  seen state. Built-ins ship as grit resources; installed plugins live in
  `<user data>/Compass Plugins/<id>/<version>/`. A folder put straight into
  `Compass Plugins` with a `manifest.json` in it is a local plugin (for
  writing one without hosting it): read again whenever the plugin list
  shows, served from where it is, never deleted by Compass, and with no
  source URL, so other devices that sync its folders can't install it.
- Refresh on folder expand, on space activation and on a per-folder timer
  (`nextRefresh` or `refresh.default`, clamped), with exponential backoff
  after errors, for loaded profiles only, paused in battery saver.

### UI

- Sidebar: the plugin's icon in place of the folder glyph, a spinner while
  refreshing, an error dot with the message, unseen dots on entries and a
  count on a collapsed folder.
- New Live Folder… beside New Folder: plugin picker, then a form built from
  `manifest.config` and `secrets`, and the cookie grant when the plugin asks.
- Folder context menu: Refresh, Edit Source…, Mark All Seen, Convert to
  Normal Folder.
- Settings › Live folder plugins: installed plugins, install from a URL or
  file (showing hosts, cookie access and secrets first), secrets, removal.
- Android: the same states in `SidebarState.kt` / `SidebarUi.kt` (the
  snapshot in `spaces_android.cc` carries them) and Compose sheets for the
  picker and form; the runtime is the shared C++ service.

### iOS

- Regenerated protos; `SidebarNode.swift` gets `liveSource`, `liveItemKey`
  and `unseen`; `SpaceStore` ports the model rules; the spaces bridge skips
  generated nodes.
- A `CompassLiveFolders` module runs each refresh in a fresh `JSContext`,
  with `compass.fetch` on `URLSession` (cookies copied from the space's
  `WKWebsiteDataStore` for granted hosts) and secrets in the Keychain.
- Refresh in the foreground, on folder expand and in the existing
  `BGAppRefreshTask`.

### Order

1. Contract, SDK, built-ins, reference runtime, cases. **Done.**
2. Desktop model and serializer, with unit tests.
3. Desktop runtime, sidebar UI, New Live Folder with the built-ins.
4. Sync.
5. Installing plugins, Settings page, cookie grants.
6. Android UI.
7. iOS.

## Known gaps

- The HTML helper is pattern based: links and titles only.
- `ics` doesn't read `VTIMEZONE` definitions; zones it can't map to an IANA
  name are read in the viewer's zone. RRULE `BYWEEKNO`, `BYYEARDAY`,
  `BYHOUR` and sub-daily frequencies are ignored.
- GitHub needs a token; there's no OAuth flow.
