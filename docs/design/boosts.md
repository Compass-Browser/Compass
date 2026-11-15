# Boosts

Per-site CSS and JavaScript that the user makes by hand or asks Claude to
make, as Arc's Boosts and Zen's Mods do. A Boost applies to every page a
site pattern covers, from the next load on, on every device of the profile.

Status: desktop (Mac, Windows, Linux) and iOS in the first version; Android
later. The desktop side is described here; iOS has its own store and
injection (`ios/App/Boosts/`, `ios/CompassKit/Sources/CompassBoosts/`) with
the same data, matching and tools.

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
| `author` | `"user"` or `"claude"`: who made it; edits don't change it |
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

## Settings

A section of chrome://settings, **Boosts** (`/boosts`, after Links from
Other Apps; `resources/sidebar/settings/boosts_page.{ts,html}` on
`CompassSettingsPage`, with the route, router, menu and main patches; see
settings.md). The Boosts are listed by site, each with a switch, its kinds
(CSS, JavaScript), a "by Claude" badge for Claude's, Edit and Delete. The
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

## Claude

- **Setting:** "Let Claude make Boosts" (`compass.claude.boosts`, off,
  doesn't sync; Claude settings). Boosts made by hand don't depend on it.
- **Tools** (`common/claude/tools.json`, scope `boosts`, `platforms:
  ["desktop", "ios"]`): `list_boosts {host?}`, `save_boost {id?, name,
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
the entity and keep going; Android leaves the account's Boosts alone. Not
in `proto_visitors.h`: code stays out of sync-internals. The server doesn't
look inside `COMPASS_SPACES` (docs/design/sync.md), so it needs no change.

## Tests

- `compass_unittests`: `BoostMatchTest.*` (parsing, refusals, URL and host
  matching, URL patterns), `NormalizeBoostTest.ChecksWhatsSaved`,
  `BoostStoreTest.*` (save, update, persist, the file's shape, limits,
  unreadable entries, sync's later-edit-wins, waiting for the file),
  `CompassSpacesSyncBridgeTest.BoostsSync` and `BoostEntitiesAreValid`.
- `compass_browsertests` (`browser/boosts/boosts_browsertest.cc`):
  `BoostsBrowserTest.CssAppliesAtDocumentStart` (the computed color at
  DOMContentLoaded), `SubdomainsAndPaths`, `IsolatedJsDoesntSeePageGlobals`,
  `MainWorldJsSeesPageGlobals`, `DisableEditAndDelete`;
  `BoostToolsBrowserTest.SaveBoost` (CSS saves at once, JavaScript asks
  every time, a denial saves nothing, fields left out are kept, deleting
  JavaScript asks) and `ListAndSetting`;
  `SettingsSectionsBrowserTest.BoostsSection` (the menu item, `?add=`,
  saving, the badge, the switch, Delete);
  `CommandBarPageHandlerTest.EmptyNewEntryQueryShowsRecentEntries` lists
  "Boost this site…".

## Not in v1

- Importing Arc's Boosts (where Arc keeps them on disk isn't known yet).
- Android.
- A real code editor (Monaco); Greasemonkey `GM_*` APIs and `@require`.
- Boosts in Incognito.
