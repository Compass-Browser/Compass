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

How to write one: [../live-folder-plugins.md](../live-folder-plugins.md).

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

A tab preview case has `"mode": "preview"`, the snapshot as `page` (or
`pageFile`) and `grantSession`; its recorded responses may give `groups`,
what the host's parse of the fetched HTML gives for a `select` (the Node
runtime has no DOM; the desktop browser test parses the file for real, so
the two must agree). Its `fetches` also have `via` ("page" or "browser")
and `redirect`. iOS skips preview cases: it has no hover cards.

## Tab previews

A plugin can also show what matters on a page in the sidebar's hover card,
where the screenshot would be: the next events on a Google Calendar tab,
the latest notifications on a GitHub tab. The manifest's `preview` section
says on which pages and what to read; the entry defines `previewTab(page,
config, compass)`. A plugin can fill folders, show previews or both
(`"folder": false` for a preview only).

```json
"preview": {
  "matches": ["https://calendar.google.com/calendar/*"],
  "select": {
    "events": {"selector": "[data-eventid]", "attrs": ["data-eventid"],
               "text": true, "fields": {"label": ".XuJrye"}, "max": 200}
  },
  "pageText": false,
  "session": {"related": ["*.googleapis.com"], "pageCookies": ["SAPISID"]},
  "config": [{"key": "count", "type": "number", "label": "Events",
              "default": 5, "min": 1, "max": 10}]
}
```

| Key | Rules |
|---|---|
| `matches` | 1–20 patterns `scheme://host/path`: scheme `http`, `https` or `*`; host or `*.domain`, never a public suffix (`*.com`, `*.github.io`) or an IP; a path where `*` is any run of characters, matched against path and query. |
| `select` | At most 10 groups: `selector` (CSS, ≤ 500 characters), `attrs` (≤ 20 attribute names; `href`, `src` and the like made absolute), `text` (the element's text, collapsed, ≤ 1,000 characters), `fields` (≤ 20 names → `"selector"`, `"selector@attribute"` or `"@attribute"`, read from the first match inside the element), `max` (1–200, default 50). |
| `pageText` | The page's text too (≤ 50,000 characters). |
| `badge` | Its result's `count` shows on the tab's row, kept current in the background ("Badges", below). |
| `session` | Advanced mode, below: `hosts` (narrows the tab's site), `related` (≤ 5 other hosts), `pageCookies` (≤ 5 non-HttpOnly cookie names). Every match must be on one registrable domain. |
| `config` | The preview's own settings form, like a folder's `config`, set in Settings › Plugins. |

`previewTab()` gets `page = {url, title, live, groups: {<group>: [{attrs,
text, fields}]}, text?, cookies?}` and returns `{items: [{id, title,
subtitle?, url?, time?, badge?, icon?}], title?, footer?, count?}`: at
most 10 items, `url` optional (a click shows the tab), `title` a header and
`footer` a line under the rows. An empty result (no items, no footer)
leaves the screenshot.

### Security model

A preview reads the page, which a folder never does, so:

1. **Compass reads the page, not the plugin.** The browser runs
   `plugins/runtime/select.js` (Compass's code) in an isolated world of the
   tab, applying the manifest's selectors; the page's scripts can't see or
   change it. The plugin runs in the same sandboxed Worker as a folder's
   refresh and gets the snapshot as data.
2. **No requests.** In a preview, `compass.fetch()` fails with
   `host_not_allowed` (or `session_not_granted` when it declares `session`),
   whatever `hosts` says, so what it read can't be sent anywhere. Limits: 2
   s, no fetches.
3. **Icons are requests too.** An item's `icon` loads only from the tab's
   site (and, with the session granted, the related hosts); others are
   dropped. `url` isn't restricted: only a click opens it.
4. **Where.** `matches` can't name browser pages or the Web Store
   (`claude::CanUsePage()`); previews never run off the record.
5. **Consent.** Built-ins' previews are on; installed plugins' are off until
   the user turns them on in Settings › Plugins, where each says which
   sites it reads.

**Advanced mode (`session`).** When the page doesn't show what's needed (a
month view, a list that loads later, an unloaded tab), a plugin may make
requests with the user's session, once the user allows it for that plugin
("Advanced: use your session on google.com", off by default, even for
built-ins). Then:

- Requests may go to the tab's **site**, subdomains included (eTLD+1 with
  the Public Suffix List's private registries, so `alice.github.io` can't
  reach `bob.github.io`), narrowed by `session.hosts`; they carry the
  user's cookies. While the tab is live they're sent from the tab's
  isolated world (`select.js`'s `startFetch()`, polled by the browser),
  with the page's origin; otherwise from the browser.
- **Related hosts** (`session.related`, for CDNs and API domains) are
  reached from the browser, never with cookies. A redirect from the site to
  one drops the cookies; a redirect anywhere else fails with
  `host_not_allowed`. An in-page request that gets a redirect is sent again
  from the browser, which checks each hop.
- `pageCookies` lets the plugin read the named non-HttpOnly cookies (to
  sign requests, as Google's APIs want); `compass.hasSession` says whether
  it may.
- `compass.fetch(url, {select: "<group>"})` (or several names) applies
  selector groups to the fetched HTML (parsed with `DOMParser`, which runs no scripts, in the
  host page, since the Worker has no DOM) and returns `groups`.
- Limits: 5 requests, 5 s, 2 MB each. The guarantee becomes: *page data can
  reach only the tab's site and the hosts listed when the user allowed it*.
  The grant is for the related hosts it was given for: an update that adds
  one asks again.

### Badges

A preview with `"badge": true` can put a notification count on its tab's
row in the sidebar: its result's `count`, a number (0 for none, at most
99,999, shown as "99+" past 99) or `true` for "some, number unknown" (a
dot). The count is the result's, whether or not the result has rows, and
needs no hover:

- `TabPreviewService::RefreshCounts()` runs every 15 s
  (`kCountTick`): for each tab of the profile whose page a badge plugin
  matches, it runs the preview again once a minute (`kCountEvery`; 5 min
  with the session, `kSessionCountEvery`), and right away for a page it
  hasn't counted yet, so a new page gets its count within 15 s. Discarded
  tabs keep their last count. Pages no longer open, or no longer matched (a
  plugin turned off, previews off), lose theirs.
- Any run of the preview (a hover too) updates the count (`counts_`, by
  page URL); `TabPreviewService::Observer::OnTabCountsChanged()` tells the
  sidebar, which sends `Page.NodeBadgeChanged(id, count)` for its tab rows
  (`Node.badge_count`: 0 none, -1 a dot).
- The sidebar shows a pill at the row's end (`.tab-badge`), a dot without a
  number, and a dot in a favorite tile's corner (`.tile-badge`). Live tabs
  only.

`compass.github` counts unread notifications: on github.com/notifications
from the Inbox counter, elsewhere (with the session) from the fetched page's
counter; without the session, the header's "You have unread
notifications" gives a dot. Tests: the `preview-count` conformance case,
`github/preview-*`, `LiveResultTest.PreviewRules`,
`CompassSidebarPreviewBrowserTest.PluginBadges`.

### Desktop

- **Runtime:** the live folders' host runs previews too (`RunRequest.preview`,
  `page_json`, `select_json`, `icon_hosts`; `worker.js` calls
  `__compassRunPreview()`). `MakeLivePreviewRunSpec()`
  (`live_folder_service.cc`) builds the run; without the session there's no
  fetcher and the browser refuses every fetch.
  `PreviewSessionFetcher` (`browser/ui/live_folders/`) implements the
  session's rules over `LiveFetcher` and the tab transport.
  `ParseLivePreviewResult()` checks the result again, icons included.
- **`TabPreviewService`** (owned by `LiveFolderService`): which plugin
  previews a URL (built-ins first), the snapshot, the run, results kept 30
  s (2 min with the session) and the last good one per page, shown for an
  unloaded tab under "Last seen 10:42". Settings live in
  `compass.tab_previews.enabled` and `compass.tab_previews.plugins`
  (profile prefs, not synced).
- **Hover card:** see docs/design/sidebar.md, "Hover cards".
- **Built-ins:** `compass.calendar` reads Google Calendar's event chips
  (English labels); `compass.github` shows recent notifications from
  `github.com/notifications`, from the page there, else (with the session)
  by fetching it.

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
- Settings › Plugins (docs/design/settings.md, "Plugins"): every plugin
  with what it does (Live folder, Tab preview), install from a URL (showing
  hosts, cookie access, secrets and the pages a preview reads first),
  previews' switches, settings and session grants, source, removal.
- Android: the same states in `SidebarState.kt` / `SidebarUi.kt` (the
  snapshot in `spaces_android.cc` carries them) and Compose sheets for the
  picker and form; the runtime is the shared C++ service.

### iOS

Done (`ios/CompassKit/Sources/CompassLiveFolders`, `ios/App/Model/LiveFolders.swift`,
`ios/App/Views/LiveFolderViews.swift`):

- Regenerated `compass_spaces_specifics.pb.swift`; `SidebarNode` has
  `liveSource`, `liveRefreshed`, `liveItemKey` and `unseen`, saved in
  `Sidebar.json`; `SpaceStore` ports the model rules (`addLiveFolder`,
  `setLiveSource`, `convertToPlainFolder`, `replaceLiveItems`,
  `markAllSeen`). `CompassSpacesBridge` writes and reads `live_source`,
  never commits an entry, drops this device's entries when the folder is
  made normal or deleted elsewhere, and keeps a source it can't apply (a
  live folder outside Pinned stays plain here without clearing it).
- `CompassLiveFolders` runs each refresh in a fresh `JSContext` on its own
  `JSVirtualMachine`. JavaScriptCore has no `URL`, `URLSearchParams`,
  `TextEncoder`, `TextDecoder` or timers: `Resources/jsc_globals.js` ports
  the WHATWG URL parser and the encodings (it passes WPT's
  `urltestdata.json`), and the host provides `setTimeout`. It has no switch
  for `eval` either, so the constructors that compile strings are replaced
  before any plugin code runs. Manifest and config validation, the
  allowlist and result normalization run `manifest.js` and `result.js` in
  the host's own context. `compass.fetch` is an ephemeral `URLSession` with
  no cookie store: redirects are checked hop by hop, bodies stop at 2 MB,
  and the Cookie header comes from the space's `WKWebsiteDataStore` only
  for granted cookie hosts. `CompassLiveFoldersTests` runs every builtin
  and conformance case.
- `LiveFolders` (the app) refreshes in the foreground (due folders, checked
  every 30 s), when a folder opens after 5 minutes and in the existing
  `BGAppRefreshTask`, two at a time, with the desktop's intervals and
  backoff. Secrets are in the Keychain per profile and plugin
  (`ThisDeviceOnly`); cookie grants are per folder in `Live Folders.json`.
  The built-ins ship as the `sdk`, `runtime` and `builtin` folders at the
  top of the app's bundle (a `plugins` folder would be its `PlugIns` on a
  case-insensitive disk); installed plugins go in `Live Plugins/<id>/`,
  from a manifest's address after a review, with the desktop's hash.
- **Installed by hand:** `Documents/Live Folder Plugins/` (Files › On My
  iPhone › Compass, made on first use with a Read Me) takes a plugin's
  folder as it is: its `manifest.json`, with the entry and icon it names
  beside it (`LivePluginPackage.manual(in:rules:)`, the same checks and
  size limits as a fetched one). It's read again whenever the app comes to
  the front; a plugin there wins over an installed one with the same ID,
  shows "Installed by hand" in New Live Folder, and Remove deletes its
  folder. Its source is the folder's file URL, so other devices syncing its
  folders can't fetch it. Folders that aren't a valid plugin are listed
  under "Not loaded from Live Folder Plugins", with why.
- Sidebar: the plugin's icon (an SF Symbol for the built-ins, whose SVG
  icons UIKit can't draw; an installed plugin's PNG), a spinner, a warning
  with the error in the menu, unseen dots and a count on a closed folder;
  New Live Folder… beside New Folder and in the space menu; Refresh, Edit
  Source…, Mark All Seen and Convert to Normal Folder.

### Order

1. Contract, SDK, built-ins, reference runtime, cases. **Done.**
2. Desktop model and serializer, with unit tests.
3. Desktop runtime, sidebar UI, New Live Folder with the built-ins.
4. Sync.
5. Installing plugins, Settings page, cookie grants.
6. Android UI.
7. iOS. **Done.**
8. Tab previews (desktop): contract, runtime, session mode, hover card,
   Settings › Plugins, Claude's plugin tools, the Calendar and GitHub
   previews. **Done.**

## Known gaps

- The HTML helper is pattern based: links and titles only.
- `ics` doesn't read `VTIMEZONE` definitions; zones it can't map to an IANA
  name are read in the viewer's zone. RRULE `BYWEEKNO`, `BYYEARDAY`,
  `BYHOUR` and sub-daily frequencies are ignored.
- GitHub needs a token; there's no OAuth flow.
- Tab previews: rows don't show item icons yet (the icon rule is enforced,
  but the hover card loads no images), and can't be clicked (the card
  takes no focus). The Calendar preview reads English labels only and has
  no session mode yet; the GitHub selectors follow today's
  github.com/notifications markup and will need updates when it changes.
  The Node runtime can't parse HTML, so `select` on fetched pages is
  checked by the desktop browser test only. Desktop only: iOS and Android
  have no hover cards.
- iOS: a plugin stuck in a synchronous loop times out, but its thread keeps
  running until the app quits (JavaScriptCore has no public way to stop a
  script). Installed plugins' SVG icons show as a bolt. A synced folder
  whose plugin isn't installed offers to install it from Edit Source…, not
  at once.
