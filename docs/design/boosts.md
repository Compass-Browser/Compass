# Boosts

Per-site CSS and JavaScript that the user makes by hand or asks Claude to
make, as Arc's Boosts and Zen's Mods do. A Boost applies to every page a
site pattern covers, from the next load on, on every device of the profile.

Status: desktop (Mac, Windows, Linux), iOS and Android. The desktop side
is described here; iOS has its own store and injection (`ios/App/Boosts/`,
`ios/CompassKit/Sources/CompassBoosts/`) with the same data, matching and
tools. Android runs the desktop's store, injector and Claude tools, and
syncs them (android.md, "Boosts"), with a plain list in place of the
editor.

## The data (shared)

A Boost is:

| Field | |
|---|---|
| `id` | a lowercase UUID; the sync client tag |
| `name` | what the user calls it; the match when left empty; at most 100 characters |
| `match` | where it applies (below) |
| `css` | inserted at document start |
| `js` | run at document idle |
| `js_world` | `"isolated"` (default) or `"main"` |
| `enabled` | |
| `author` | `"user"`, `"claude"` or `"shared"` (added from someone's link or file, see Sharing): who made it; edits don't change it; a value a client doesn't know is kept |
| `create_time`, `update_time` | microseconds since the Windows epoch, as `CompassMemory`'s |

Limits: 256 KB of CSS and JavaScript together per Boost, 200 Boosts per
profile (sync isn't capped: one dropped there would be deleted everywhere).
A Boost needs CSS, JavaScript or both.

**Match.** A host pattern, optionally followed by a path prefix, http and
https pages only, any port:

- `example.com` covers `example.com` and `www.example.com`;
- `*.example.com` covers `example.com` and all its subdomains;
- `github.com/settings` covers paths that start with `/settings` (a plain
  prefix: `/settingsfoo` too).

`BoostMatch::Parse()` is lenient with what people paste: a scheme
(`https://`), a leading `www.` (which the site covers anyway), trailing `/`
and `*` are taken off; the host is lowercased and IDN becomes punycode.
Refused: other schemes, ports, user info, `*` anywhere else, `?` and `#`,
and `*.` on a single label or an IP address (`*.com` would be most of the
web). The normalized pattern is what's stored and synced.

**Store** (`browser/boosts/boost_store.{h,cc}`, target `:store`):
`BoostStore`, a `KeyedService` per profile (`BoostStoreFactory`; none for
Incognito, Guest or system profiles), in `<profile>/Compass Boosts.json`: a
JSON array of objects with the snake_case keys above (times as strings, as
`base::TimeToValue()` writes them). Modeled on `ClaudeMemory`: read once on
a background sequence, written by `ImportantFileWriter` a second after a
change, an observer list, `Save()` (no id: new, made now; an id: an update,
keeping author and creation time), `SetEnabled()`, `Remove()`, `Restore()`
(Undo) and `ApplyRemote()` (sync; the later `update_time` wins; waits for
the file).

## Injection

`BoostInjector` (`browser/boosts/boost_injector.{h,cc}`, target
`:boosts`), a `KeyedService` created with the profile, runs the Boosts
through Chromium's content scripts, so CSS at document start, isolated and
main worlds, every frame and updates without a restart come from upstream:

1. Once the extension system is ready, it adds the **"Compass Boosts"
   component extension** (`resources/boosts/manifest.json`, embedded with
   `build/embed_text.py`; id `bmicmjdbmhbbcjkbfnjbphgocpjlfjbk` from its
   `key`) through `ComponentLoader::Add()`, with its root at
   `<profile>/Compass Boosts Files`. MV3, `host_permissions: <all_urls>`, no
   background, no UI; component extensions are hidden from
   chrome://extensions. `ComponentLoader` only loads allowlisted component
   extensions: `patches/chrome-browser-extensions-component_extensions_allowlist-allowlist.cc.patch`
   adds the id.
2. For each enabled Boost it registers up to two **dynamic content scripts**
   (`UserScript::Source::kDynamicContentScript`, ids `_dc_compass-boost-…`)
   on the extension, through `UserScriptManager` →
   `ExtensionUserScriptLoader::AddPendingDynamicScriptIDs()` /
   `AddDynamicScripts()` / `RemoveDynamicScripts()`: the CSS at
   `document_start`, the JavaScript at `document_idle` in
   `ExecutionWorld::kIsolated` or `kMain`; `match_all_frames`; never in
   Incognito; URL patterns from `BoostMatch::ToUrlPatterns()`
   (`*://example.com/*` and `*://www.example.com/*`, or
   `*://*.example.com/*`, the path prefix as `/settings*`). Not persistent:
   the store is the source, registered again at each start.
3. **Why files.** The plan was inline content (`UserScript::Content` with
   the code set). Upstream's loader doesn't keep inline content for CSS:
   `UserScript::CopyMetadataFrom()` drops all content, and
   `CopyDynamicScriptInfo()` copies it back only for JavaScript (the
   userScripts API has no inline CSS), so the loader would read the CSS
   from a file that doesn't exist. So each script is a file in the
   extension's root, named after the Boost, its kind and a hash of its
   content, world and patterns (`<id>-css-<hash>.css`), and registered with
   `Content::CreateFile()`. A change makes another file and another script
   id; the old one is unregistered, and files no registered script uses are
   deleted after each pass.
4. **Updates.** On every `OnBoostsChanged()` it computes the scripts the
   enabled Boosts need, removes those that went and adds those that came,
   one pass at a time (a change during a pass starts another after it).
   Pages loaded from then on get the change; open pages keep what they had
   until they reload. `WhenRegistered()` runs a callback once what's
   registered matches the store: Claude's tools wait for it, so a reload
   right after a save gets the Boost; the browser tests too.

Upstream's `UserScriptListener` holds navigations at startup only for
scripts registered while the extension loads, not for dynamic ones: a tab
restored at startup before the first pass ends loads without its Boosts
until it reloads. The extension's total script size is upstream's limit for
one extension; past it, scripts load without their content (logged).

## Zap

Arc's Zap: "Zap elements on this site…" in the command bar
(`kZapSite`, for web pages of profiles with Boosts) puts a bar at the
bottom of the page (`browser/ui/zap/zap_page.js`, in the main frame's
isolated world, a top-layer popover in a closed shadow root, as Capture to
Easel's overlay):

- hovering outlines the element under the pointer; clicking hides it at
  once (inline `display: none !important`), and the page doesn't get the
  click;
- **Undo** shows the last one again; **Done** or Enter keeps them; Esc
  puts them all back and keeps nothing.

Each hidden element gets a selector: its id when that's unique and doesn't
look generated (no runs of digits or long upper-case hashes), else steps up
the tree (tag, up to two stable classes, `:nth-of-type` when siblings need
it) until it's unique or reaches an ancestor with a good id.

`ZapSession` (`browser/ui/zap/zap_session.{h,cc}`) polls the page as
Capture to Easel does, and on Done saves the selectors to the site's **zap
Boost** (`browser/boosts/zap.{h,cc}`): an ordinary Boost for the host
(`BoostMatch` of the page's host), named "Zapped on <host>", whose CSS
starts with `/* Zapped with Compass */` and has one
`<selector> { display: none !important; }` rule per element. Zapping the
site again adds to it (no duplicates) and turns it back on; it syncs,
shows in Settings and is edited or deleted like any Boost. Selectors with
braces, comments or newlines are dropped, so each stays one rule. A toast
says where they went.

## Settings

A section of chrome://settings, **Boosts** (`/boosts`, after Links from
Other Apps; `resources/sidebar/settings/boosts_page.{ts,html}` on
`CompassSettingsPage`, with the route, router, menu and main patches; see
settings.md). The Boosts are listed by site, each with a switch, its kinds
(CSS, JavaScript), a "by Claude" badge for Claude's and "Shared" for
shared ones, Share, Edit and Delete; Import… is next to Add Boost (see
Sharing). The
editor has the name, the site pattern (with what it covers), the CSS and
the JavaScript in monospace text areas (Tab indents) and where the
JavaScript runs. `?add=<host>` opens the editor on a host (or its Boost if
it has one), `?edit=<id>` on a Boost. The list is read again when the page
becomes visible (Claude and sync change it too).

It uses the settings' `SettingsHandler` (`sidebar.mojom`: `GetBoosts()`,
`SaveBoost()`, `SetBoostEnabled()`, `DeleteBoost()`, implemented by
`ShortcutsPageHandler`, which the Links section uses too), so it needs no
broker entry of its own. Standalone, the same element is at
chrome://compass-settings/boosts.html (not a host of its own: the
compass-settings host already binds the handler).

**Boost this site…** is a command bar command (`kBoostSite`, for an http or
https page, not off the record): it opens `/boosts?add=<host>`. Not done: a
"Boost this site…" item in the sidebar's page menu, and the plan's icon in
the header that shows Boosts are active on the page and turns them off for
the tab (it needs per-tab state the content scripts don't have; a later
version could match the tab's URL against the store and reload without the
extension's scripts).

## Sharing

A Boost can be shared as a link or a file and added by whoever opens it,
on any Compass client. The format is the same everywhere (desktop:
`browser/boosts/boost_share.{h,cc}`, target `:share`; iOS has its own
codec), and every client tests against the vectors in
`common/boosts/share_vectors.json`.

**Shared JSON**, UTF-8:
`{"v":1,"name":…,"match":…,"css":…,"js":…,"js_world":"isolated"|"main"}`.
`css`, `js` and `js_world` may be absent (desktop leaves out an empty
`css` or `js`, and `js_world` without `js`). No id, author, times or
`enabled`: those belong to the profile that adds it. Readers ignore keys
they don't know; a `v` other than 1 is refused ("This Boost was made by a
newer Compass"). Key order and escaping don't matter (desktop's JSON writer
escapes `<` as `<`; iOS doesn't escape `/`).

**Link:** `https://compassbrowser.org/boost#` followed by base64url
(RFC 4648 §5, no padding; readers accept padding) of the raw DEFLATE
(RFC 1951, no zlib or gzip header) of the compact JSON. The payload is only
in the fragment, which browsers never send, so it never reaches a server.
Readers also accept `www.compassbrowser.org`, `/boost/` and `/boost.html`.
A link over 64 KB isn't offered: share a file.

**Test vectors** (`common/boosts/share_vectors.json`): a JSON array. Each
entry has a `name` and a `fragment`. One that decodes also has `json`, the
JSON its fragment holds (as a string), and `expected`, the Boost it adds
once normalized; encoders may write other bytes, so tests compare by
decoding. One that must fail has `error` instead: `newer` (`v` isn't 1),
`too_big` (over 512 KB decompressed), `invalid` (not a shared Boost) or
`refused` (the rules for saving a Boost).

**File:** `<name>.compassboost` (characters file systems refuse become
`-`), the same JSON pretty-printed, uncompressed. MIME type
`application/vnd.compassbrowser.boost+json`, UTType
`org.compassbrowser.boost`.

**Adding one** is the same as saving a Boost (`NormalizeBoost()`: the match
normalized, the 100-character name, a Boost needs CSS or JavaScript, 256 KB
of code, the 200-Boost count), and more than 512 KB decompressed (or a file
over 512 KB) is refused before parsing, as a decompression bomb guard. It
always goes through a preview the user confirms: the name, the site, the
full CSS and JavaScript as text, and, with JavaScript, "This Boost runs
JavaScript on <site>. Only add it if you trust whoever shared it." and
which world it runs in. Confirming adds a **new** Boost (a new id, on,
author `"shared"`); it never replaces one, so the same link twice gives two.
After that it is like any other Boost: it syncs, and the user can edit it.
Claude's approval doesn't look at the author: changing a shared Boost's
JavaScript asks as usual. Claude has no tool to share or import.

**Desktop.**

- *Share* in a Boost's row opens Copy Link (hidden, with a note, when the
  link would be too long) and Save File… (a save dialog in Downloads).
  `SettingsHandler`: `GetBoostShareLink()`, `CopyBoostShareLink()` (the
  browser writes the clipboard), `SaveBoostFile()`.
- *Links.* `BoostLinkInterceptor` (`boost_link_interceptor.{h,cc}`, target
  `:link`), a tab helper attached by upstream's `TabHelpers` (the existing
  `patches/chrome-browser-ui-tabs-tab_features.cc.patch` and
  `chrome-browser-ui-BUILD.gn.patch` call and depend on it), watches each
  tab's primary main frame navigations (start and redirects). A share link
  is stopped before it loads, and `chrome://settings/boosts?import=<payload>`
  opens instead: in the same tab if it had nothing committed yet (a link
  from another app, a new tab), else in a new foreground tab next to it, so
  the page with the link stays. Both renderer-initiated (a link in a page)
  and browser-initiated (typed, from another app) navigations are caught. A
  payload with characters outside base64url gives an empty `import=`, which
  the page shows as a broken link.
- *Incognito.* The preview opens through `WebContents::OpenURL()`, and
  `chrome::Navigate()` never opens settings off the record: it opens it in
  a normal window of the original profile (a singleton tab), whose Boosts
  get it. Nothing about the Boost stays in the incognito tab.
- *Import…* opens a file dialog (`.compassboost`, or any file);
  `ChooseBoostFile()` reads at most 512 KB on a background thread, checks
  it, and hands the page the link payload for it, so files and links share
  the preview (`PreviewSharedBoost()`, `AddSharedBoost()`, which decode and
  check the payload again).
- Not done: opening a `.compassboost` file with Compass (it needs file
  associations in each installer and the Mac Info.plist) and dropping one
  on a window (it would need upstream's drop handling patched). Import…
  covers both.

**Website.** `website/boost.html` (`/boost`) reads `location.hash` in the
page (`boost.js`, with `DecompressionStream('deflate-raw')` and the same
512 KB guard), shows the name, site, CSS and JavaScript as text only, never
runs or inserts them, and says to open the link in Compass, with download
links. Without a fragment, or with a broken one, it explains Boosts. It
makes no requests (its CSP has `connect-src 'none'`) and has no analytics.
`tools/tests/test_website_boost.py` runs `boost.js` on the shared vectors
in Node.

## Claude

- **Setting:** "Let Claude make Boosts" (`compass.claude.boosts`, off,
  doesn't sync; Claude settings). Boosts made by hand don't depend on it.
- **Tools** (`common/claude/tools.json`, scope `boosts`, `platforms:
  ["desktop", "android", "ios"]`): `list_boosts {host?}`, `save_boost {id?, name,
  match, css?, js?, js_world?, enabled?}` (returns `{"id", "needs_reload":
  true}`), `delete_boost {id}`. Sent only while the setting is on, and kept
  for the rest of a chat that had them (`chat_boosts_`, as the developer
  tools); `ClaudeTools::RunBoostTool()` (`browser/claude/claude_boost_tools.cc`)
  checks the setting at each call. They need no tab. Not offered to MCP
  clients or in Claude Code mode yet.
- **Approval.** A save that leaves the Boost with JavaScript to run that
  the user hasn't allowed (new code, another world, another match, or a
  disabled Boost with JavaScript turned on) and deleting a Boost with
  JavaScript ask the user first through `ApproveThen()`, showing the code
  (and the code it replaces), up to 12,000 characters. These requests have
  `ApprovalRequest::always_ask`: "Allow for this chat" allows them once and
  doesn't cover the next. Taking JavaScript out never asks.
- **Undo.** CSS-only changes save at once. Every change shows in the chat
  as "Saved Boost for github.com" (or "Updated", "Deleted") with **Undo**,
  like memory's line: the result's `boost: {kind, text, undo}`
  (`ToolResultJson()`), `chat.mojom` `UndoBoost()`, which puts the previous
  version back (`Restore()`) or removes a new one.
- **After a save** the tool waits until the injector has the change
  (`SetBoosts()`'s `when_applied`), then answers; Claude reloads the tab
  (navigate) to check the result, as the system prompt's "Boosts" section
  says, with when to make one (a lasting change, not a one-off `run_js`),
  CSS first, small site-specific scripts, and never sending page data
  anywhere.

## Sync

`CompassBoost`, field 7 of `CompassSpacesSpecifics`
(`components/sync/protocol/compass_spaces_specifics.proto`), in the account
of the profile it belongs to, end-to-end encrypted like the rest, under the
Spaces switch. The bridge handles it as it does memories:
`Delegate::GetLocalBoosts()` (nullopt until the file is read, so a startup
never deletes the account's) and `ApplyBoosts()`; the later `update_time`
wins. A deleted Boost is a deleted entity. Clients that predate it refuse
the entity and keep going; Android syncs them as the desktop does
(`compass_spaces_sync_android.cc`). Not
in `proto_visitors.h`: code stays out of sync-internals. The server doesn't
look inside `COMPASS_SPACES` (docs/design/sync.md), so it needs no change.

## Tests

- `compass_unittests`: `BoostMatchTest.*` (parsing, refusals, URL and host
  matching, URL patterns), `NormalizeBoostTest.ChecksWhatsSaved`,
  `BoostStoreTest.*` (save, update, persist, the file's shape, limits,
  unreadable entries, sync's later-edit-wins, waiting for the file),
  `CompassSpacesSyncBridgeTest.BoostsSync` and `BoostEntitiesAreValid`,
  `BoostShareTest.*` (the shared vectors, round trips, the JSON's keys,
  refusals, the 512 KB guard, link URLs and file names).
- `compass_browsertests` (`browser/boosts/boosts_browsertest.cc`):
  `BoostsBrowserTest.CssAppliesAtDocumentStart` (the computed color at
  DOMContentLoaded), `SubdomainsAndPaths`, `IsolatedJsDoesntSeePageGlobals`,
  `MainWorldJsSeesPageGlobals`, `DisableEditAndDelete`;
  `BoostToolsBrowserTest.SaveBoost` (CSS saves at once, JavaScript asks
  every time, a denial saves nothing, fields left out are kept, deleting
  JavaScript asks), `SharedBoostAsksForJs` and `ListAndSetting`;
  `BoostShareBrowserTest.LinkInAPageOpensThePreview` (a new tab with the
  preview, the page left alone, confirming adds a shared Boost, twice gives
  two), `LinkInANewTab`, `BrokenLink`, `IncognitoOpensANormalWindow` and
  `ShareCopiesALinkThatDecodes`;
  `SettingsSectionsBrowserTest.BoostsSection` (the menu item, `?add=`,
  saving, the badge, the switch, Delete);
  `CommandBarPageHandlerTest.EmptyNewEntryQueryShowsRecentEntries` lists
  "Boost this site…".

Zap: `ZapTest` (the CSS, adding to a site's zap Boost once, selectors
that would break a rule, other Boosts) and `ZapBrowserTest` (a click
hides, Enter keeps it hidden after a reload, Esc puts it back).

## Not in v1

- Importing Arc's Boosts (where Arc keeps them on disk isn't known yet).
- An editor on Android (Claude makes them there; the list turns them on
  and off and deletes them).
- A real code editor (Monaco); Greasemonkey `GM_*` APIs and `@require`.
- Boosts in Incognito.
