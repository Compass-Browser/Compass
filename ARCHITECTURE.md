# Compass architecture

Compass is an open-source, cross-platform browser built on a full Chromium
fork. It keeps Chromium's engine, extension system and security model, and
replaces the tab strip with an Arc-style sidebar: spaces, profiles, pinned
tabs, nested folders, split view, window sync and automatic picture-in-picture.

## 1. Goals and non-goals

**Goals**

- macOS, Windows and Linux from one codebase.
- Full Chromium: Chrome Web Store extensions, DevTools, autofill, password
  manager, every web platform feature.
- A clean, fast, opinionated UI: a vertical sidebar instead of a tab strip.
- Follow Chrome stable within days, not weeks, including security fixes.

**Also in this repository**

- Cross-device sync. Chrome Sync is unavailable to third-party builds, so
  Compass has its own end-to-end encrypted, self-hostable service
  (`server/`, [docs/design/sync.md](docs/design/sync.md)).
- Compass for iOS, a native SwiftUI + WebKit app that syncs with the desktop
  (`ios/`, [docs/design/ios.md](docs/design/ios.md)).

**Not yet**

- Widevine VMP signing (Widevine itself works, unverified: [docs/design/widevine.md](docs/design/widevine.md)).

## 2. Base: a pinned Chromium stable

- `config/chromium_version` pins one Chromium stable tag for every platform.
- `compass update-chromium <version>` moves to a new tag: revert patches, fetch,
  sync DEPS, re-apply patches with a 3-way merge, report conflicts.
- Chrome stable moves every 4 weeks, with security respins in between. Each
  rebase is a routine task, so it has to stay cheap. That's what the next
  section is about.

## 3. How we modify Chromium

Three mechanisms, in order of preference. Use the first one that works.

| # | Mechanism | Where | When |
|---|---|---|---|
| 1 | **New code in `//compass`** | `browser/`, `resources/`, … | All features. Most code lives here. |
| 2 | **`chromium_src` overrides** | `chromium_src/<path in src>` | You need to change upstream behaviour without editing the file: wrap, rename or extend a function. |
| 3 | **Patches** | `patches/*.patch` | Small edits that can't be done any other way: BUILD.gn hooks, a single line in a header, branding. |

### Layout on disk

```
~/src/compass/            ← this repository (small; open it in your IDE)
~/src/compass-chromium/   ← created by `compass init` (~100 GB with builds)
  ├─ .gclient
  ├─ depot_tools/
  └─ src/                 ← Chromium checkout at the pinned tag
      └─ compass → ~/src/compass   (symlink)
```

The repository directory must be named `compass` in lowercase. Includes are
spelled `compass/...`, and clang rejects a differently cased directory behind
the link (`-Wnonportable-include-path`). `compass init` and `compass gen` check
this.

The Chromium checkout lives next to the repository, not inside it. Nesting it
would create a symlink cycle that IDE indexers and file watchers follow
forever. Override the location with `COMPASS_CHROMIUM_DIR`.

The CLI also supports the Brave-style in-tree layout: if this repository is
cloned directly to `<checkout>/src/compass`, it detects that and uses it as is.
Use that layout if a Chromium build script rejects a source path that
resolves outside `src/`. On Windows it's required (see
[docs/building.md](docs/building.md)).

### chromium_src overrides

Every C/C++/Objective-C compile goes through `build/redirect_cc.py`, set as
GN's `cc_wrapper`. For each compile it does two things:

1. If `chromium_src/<path>.cc` exists for the file being compiled, it compiles
   the override instead. The override usually `#define`s a hook and then
   includes the original as `#include "src/<path>.cc"`.
2. It puts `chromium_src/` first on the include path, so
   `chromium_src/<path>.h` takes precedence over `src/<path>.h` everywhere.

Ninja tracks overrides through the depfile once they're in use. Adding or
deleting an override isn't visible to ninja, so `compass build` touches the
matching upstream file to force a rebuild. Details and an example are in
[`chromium_src/README.md`](chromium_src/README.md).

### Patches

One patch per upstream file, named after its path
(`chrome/browser/BUILD.gn` → `chrome-browser-BUILD.gn.patch`). Edit the file in
`src/`, then run `compass patches export`. Patches for nested repositories
(`v8`, `third_party/skia`, …) live in a matching subdirectory of `patches/`.
Keep each patch to a few lines that are unlikely to conflict: add a hook that
calls into `//compass`, rather than moving logic into the patch.

## 4. UI architecture

Arc writes a native UI per platform (SwiftUI on macOS, Swift with custom
bindings on Windows). We can't afford three UI codebases, so we use the two
layers Chromium already ships on every platform:

| Layer | Used for | Why |
|---|---|---|
| **Views (C++)** | Window frame, content area, split panes, PiP window, popups and anchors | Native windowing, input and accessibility; fast; already cross-platform |
| **WebUI (TypeScript + Lit)** | Sidebar, command bar, space switcher, settings, onboarding | One codebase, CSS animations, fast iteration; talks to C++ over Mojo |

WebUI surfaces are hosted inside Views, the same way Chrome hosts its side
panel and tab search. Platform polish goes in small native shims: macOS
vibrancy (`NSVisualEffectView`) and traffic-light placement, Windows 11 Mica,
and Linux CSD/SSD handling.

Chrome 156 already has a vertical tab strip region
(`chrome/browser/ui/views/frame/vertical_tab_strip_region_view.h`). We put our
WebUI sidebar in that slot instead of building a new frame layout.

Performance rules for WebUI surfaces:
- Never block on C++. Every Mojo call is async, and the UI renders optimistic
  state.
- Send diffs, not whole trees.
- Virtualize long lists.
- Keep the sidebar renderer alive across space switches.

## 5. Data model

```
Profile                         (Chromium profile: cookies, extensions, passwords, history)
 └─ Space                       (owns one sidebar tree; many spaces can share a profile)
     ├─ Pinned root             (persistent: folders, tabs, splits; folders nest)
     └─ Today root              (open tabs and splits; persisted, auto-archived)

SidebarNode = Folder { title, children: SidebarNode[] }
            | Tab    { url, title }
            | Split  { panes: Tab[2..N] }     ← one entry (see §9)
```

Invariants (enforced by `SpaceModel::CanMove`):

- Folders live only under the pinned root, and can nest.
- Splits contain only tabs, at most `kMaxSplitPanes`.
- A split that drops to one pane is replaced by that pane, in the same position.
- The two roots can't be moved or removed.

The model knows nothing about `WebContents` or windows. It's a plain tree that
UI and persistence observe. Live state (which node has a `WebContents`, and
which window hosts it) is kept separately in the window-sync layer (§7).

Code: `browser/sidebar/` (`SidebarNode`, `SpaceModel`, unit tests).

Chromium's `TabStripModel` isn't the source of truth. It's a flat list with one
level of groups, so it can't represent nested folders, unloaded pinned entries
or archived tabs. It becomes an internal detail that holds the *live* tabs of
one (window, space). We keep its order aligned with the sidebar so that
extension tab indices stay meaningful.

Persistence: one JSON file per profile (`Compass Sidebar.json` in the profile
directory), written atomically and in batches by `ImportantFileWriter` on a
background sequence, the way upstream stores bookmarks (D12). It stores the
pinned tree and Today. The browser-wide list of spaces is in
`Compass Spaces.json` in the user data folder (D17).

## 6. Spaces and profiles

In `//chrome`, a `Browser` (window) belongs to exactly one `Profile`. Arc lets
one window switch between spaces that use different profiles. We get the same
result without patching that invariant:

- **One `Browser` per (native window, space).** A space switch swaps which
  `Browser`'s content is visible in the native window, with an animation.
  The user sees one window.
- Spaces that share a profile also get their own `Browser`. This keeps
  per-space history and focus clean.
- `Browser` objects for spaces that haven't been visited yet are created on
  first switch, not at startup.

Spaces that share a profile realize this by rebinding (D15): the window keeps
its `Browser`, and a switch moves the live tabs of the space it leaves into a
hidden parking `Browser` and those of the space it enters back. Spaces with
another profile swap `Browser`s inside the same native window (D17, D18). See
[docs/design/spaces.md](docs/design/spaces.md).

We rejected two alternatives: patching `Browser` to host tabs from several
profiles (invasive, conflicts on every rebase), and building on `//content`
directly (we'd lose extensions, autofill and the password manager).

## 7. Window sync

Every window shows every open tab of the active space. Windows are views onto
the shared space model, not owners of tabs.

- **One `WebContents` per tab, ever.** We never duplicate a tab per window.
  That would fork page state: forms, scroll position, media.
- **Move on focus.** Selecting a tab that is live in window A from window B
  *moves* its `WebContents` into window B. This uses the same detach/insert
  path as dragging a tab between windows, and it never crosses profiles
  because a space has one profile. Media, PiP and form state survive the move.
- Window A falls back to its previous selection. If it has none, it shows a
  placeholder: "Open in another window · Bring here".
- **Per-window state** is only: active space, the selected node per space, and
  selection history. Everything else is shared.
- **Closing a window doesn't close tabs.** Tabs belong to the space. A
  closing window hands its live tabs to another window of the profile (D13).
  We don't hold detached `WebContents` ourselves, because extensions assume
  every tab has a `windowId`. The last window of a profile, or quitting,
  closes the tabs, but their Today entries stay, unloaded. Auto-archive and
  tab discarding apply to parked tabs as usual.
- On launch, open one window on the last active space. All tabs appear in the
  sidebar and load when clicked. Tabs that Chrome's session restore brings
  back take over their saved entries (D14).

Code: `browser/window_sync/`. This includes `LiveTabRegistry`
(node → `WebContents`, host `Browser`) and the move-on-focus controller.

## 8. Extensions

Chrome Web Store installs work in a fork without changes. The work is in
mapping our model onto what extensions expect:

| Area | Behaviour |
|---|---|
| `chrome.tabs` / `chrome.windows` | Extensions see real Chromium windows and tab strips. Tab moves fire `onDetached`/`onAttached`. |
| Parking browser | Hidden from `windows.getAll` and `tabs.query` by default (patched filter), so extensions don't see a phantom window. |
| Tab indices | Match the live `TabStripModel` order, which follows sidebar order. |
| `tabGroups` | Not mapped to folders, which are a different concept (nested, persistent). Reported as unsupported at first. |
| Action icons and popups | An extensions menu in the sidebar footer, with real Views anchors so popups position correctly. |
| `sidePanel` API | Supported. It uses Chromium's side panel on the opposite side from the sidebar. |

Before each release, run a compatibility pass over the top 50 extensions
(password managers, ad blockers, Vimium, tab managers).

## 9. Split view

**A split is one entry.** It's one sidebar row, it moves as a unit between
windows, and you pin, archive or drag it as a unit.

- **Creating a split:** drop tab B onto tab A, or choose "Split with…". Both
  become panes of a new Split node in A's position, and B's own row
  disappears.
- **Closing a pane:** when one pane is left, the split is replaced by that tab.
- **Pulling a pane out:** dragging a pane to the sidebar makes it a standalone
  entry again.
- **Pinned splits:** store each pane's URL plus the layout. Unloading a split
  unloads every pane.
- **Auto-archive** uses the split's last-active time.
- **Focus:** the omnibox, find bar, zoom and permission prompts follow the
  focused pane. The focused pane is clearly marked. Extensions see the focused
  pane as the active tab.

Chrome 156 already ships split tabs (`components/tabs/public/split_tab_data.h`,
`chrome/browser/ui/views/frame/multi_contents_view.h`) with two panes. We build
on them, extended by patches to four (`kMaxSplitPanes`,
`split_tabs::kMaxTabsInSplit`): the tab strip model takes splits of up to four
tabs, and `MultiContentsView` lays them out in a row or a column with a resize
area between each two. Drop a tab on a split's pane to add it there.

## 10. Picture-in-picture

Chrome 156 has auto-PiP for tab switches
(`chrome/browser/picture_in_picture/auto_picture_in_picture_tab_helper.h`).
We extend its triggers and change how the window floats.

**Triggers:** tab switch, space switch, window blur or minimize, app
deactivation, and a tab moving to another window. We pick the most likely
video: playing, audible, largest visible area, most recently interacted with.

**Floating over everything:**

| Platform | How | Limit |
|---|---|---|
| macOS | PiP widget becomes a non-activating `NSPanel`, collection behaviour `canJoinAllSpaces \| fullScreenAuxiliary`, floating level (patch in `NativeWidgetMac` for PiP windows only) | None. Floats over other apps' fullscreen Spaces. |
| Windows | `WS_EX_TOPMOST` | Not over exclusive-fullscreen games |
| Linux X11 | `_NET_WM_STATE_ABOVE` | None |
| Linux Wayland | No standard always-on-top protocol | Compositor-dependent. GNOME won't allow it. Documented as a limitation. |

Code: `browser/ui/views/picture_in_picture/`, plus patches to upstream's
auto-PiP helpers and overlay window. See
[docs/design/picture-in-picture.md](docs/design/picture-in-picture.md).

## 11. Other Arc-style features

- **Command bar:** WebUI overlay over omnibox suggestions, tabs, spaces,
  history and commands ([docs/design/command-bar.md](docs/design/command-bar.md)).
- **Peek and little windows:** short-lived popup windows that can be
  promoted to a full tab ([docs/design/peek.md](docs/design/peek.md),
  [docs/design/little-window.md](docs/design/little-window.md)).
- **Auto-archive:** per space, into a per-space archive
  ([docs/design/sidebar.md](docs/design/sidebar.md), "Today auto-archive").
- **Import from Arc:** profiles, spaces and sidebar
  ([docs/design/arc-import.md](docs/design/arc-import.md)).
- **Boosts** (not started): per-site CSS/JS injection, planned as a built-in
  component extension so it reuses Chromium's content-script machinery.

## 12. Build configurations

`compass gen <config>` writes `out/<Config>/args.gn`, which imports
`config/args/common.gn` plus `config/args/<config>.gn`, and adds the
`cc_wrapper` line.

| Config | Use | Notes |
|---|---|---|
| `debug` | Daily development | Component build, full symbols for our code |
| `fast` | Trying it by hand | Optimized, no DCHECKs, still a component build |
| `portable` | A build to copy to another machine | `fast`, linked statically |
| `release` | Shipping | `is_official_build`, no PGO yet (`chrome_pgo_phase = 0`) |

Proprietary codecs (H.264/AAC) are on. Distributors are responsible for any
codec licensing in their region; this is the same position as other Chromium
forks.

## 13. Design docs

Each feature has a design doc in [docs/design/](docs/design/) with its
status, the code it touches and its known gaps. Start there before changing
a feature.

## 14. Decision log

| # | Decision | Rationale |
|---|---|---|
| D1 | Full Chromium fork, not Electron or CEF | Full extension support, signing in to Google sites works (browser sign-in doesn't: there's no Google OAuth client, see docs/design/privacy.md), our own security cadence |
| D2 | Views shell + WebUI surfaces, not native UI per OS | One UI codebase; proven by Chrome's side panel and Vivaldi |
| D3 | New code in `//compass`, then `chromium_src`, then patches | Keeps rebases cheap |
| D4 | Checkout next to the repo, linked in | Avoids symlink cycles; the repo stays IDE-friendly |
| D5 | Sidebar tree is the source of truth, not `TabStripModel` | Nested folders, unloaded pinned entries, archive |
| D6 | One `Browser` per (window, space) | Spaces with different profiles without patching `Browser` |
| D7 | Tabs belong to spaces; windows are views; move on focus | Window sync without duplicating page state |
| D8 | Hidden parking `Browser` per profile | Extensions always see a `windowId` |
| D9 | A split is one entry | Moves, pins and archives as a unit; simple sync semantics |
| D10 | Sync and Widevine VMP deferred until the sidebar, window sync and spaces worked | MVP first |
| D11 | Pin one Chromium stable tag for all platforms | Reproducible builds; one rebase per cycle |
| D12 | Sidebar persisted as JSON via `ImportantFileWriter`, not SQLite | The pinned tree is small and cheapest to write whole; atomic and proven by bookmarks. SQLite comes in when archive and history need queries |
| D13 | A closing window hands its tabs to another window of the profile; no parking `Browser` yet (refines D8) | A hidden window would come back visible through session restore and keep the process alive without UI on Windows and Linux. A real window gives extensions a `windowId` too |
| D14 | Today is saved in the sidebar file; tabs Chrome restores take over saved entries by URL | Chrome's session restore keeps history and crash recovery, so we leave it alone. Tagging tabs through session "extra data" would need two patches (a rebuild of the session file drops tab extra data). URL matching is right for the common case and degrades to a duplicate entry |
| D15 | Spaces of one profile share a window's `Browser`; a switch moves live tabs to and from a hidden, unrestored popup parking `Browser` (realizes D6 for one profile) | A native window can't host several `Browser`s, and swapping windows flickers, splits window state and can't restore a window's space without session patches. Moving tabs keeps page state, as tab dragging does |
| D16 | Entries move between spaces as the same node objects (`SpaceModel::MoveToSpace()`), not as copies | Node IDs and the registry's node ↔ tab pairs stay valid, so live tabs follow their entry without being closed and reopened |
| D17 | As in Arc, spaces form one browser-wide list, each assigned to a profile (`SpaceDirectory`, `Compass Spaces.json`; entries stay in their profile's file). A space of another profile swaps the window: a window of that profile opens at the same bounds and the old one empties into its profile's parking window. Assigning another profile moves the entries and reloads the shown page there. Listed profiles stay loaded (`kCompassSpaces` keep-alive) | A `Browser` has one profile (§6), so rebinding can't cross profiles, and tabs can't change profile. Without the keep-alive, a profile whose last window swapped away would be destroyed, with its parked tabs |
| D18 | A switch to another profile's space keeps the native window: the other profile's `Browser` is built hidden, then the two windows' `BrowserView`s exchange native windows (`compass::BrowserViewExchange`, `Widget::ExchangeContentsWith()` patch); the old view empties into its parking window and closes with the hidden one (supersedes D17's visible window swap) | Same NSWindow, macOS Space and fullscreen, no second window flashing. Re-profiling a live `Browser` was ruled out: ~15 window features and the views built on them cache the profile at construction (docs/design/profiles-in-place.md, "Spike findings"). Every `Browser` keeps its profile, so no upstream bookkeeping changes |
| D19 | The user's shortcuts are a Local State pref (`compass.shortcuts`, `compass::ShortcutSettings`), not a Compass JSON file, and on macOS are applied over upstream's tables at run time (AcceleratorsCocoa's map, the main menu's items, a hook in `CommandForKeyEvent()`) | Shortcuts are browser-wide, small and needed synchronously while the main menu is built, which Local State is (it's loaded by then), with no file of our own to load and write. Rewriting upstream's tables keeps one source for menus, key handling and the shortcuts upstream's menus show (docs/design/shortcuts.md "Customizing") |
| D20 | No bookmarks, no reading list and no horizontal tabs: pinned tabs are the bookmarks and the sidebar is the tab strip. `browser_defaults::bookmarks_enabled` is `false` (bar, star, Cmd+D, manager), `compass::IsRemovedCommand()` makes their commands, the reading list's and `IDC_TOGGLE_VERTICAL_TABS` unsupported in `BrowserCommandController`, the menus leave them out (the macOS Bookmarks menu is hidden, not removed, as `AppController` keeps a bridge on it), `chrome://bookmarks` isn't registered and the command bar has no bookmark provider. The `BookmarkModel`, the reading list model and their sync types stay, without switches on the sync page | One flag and one command filter cover every shortcut, menu and caller. Keeping the model avoids patching the many upstream services that assume it, and keeps bookmark sync (and the sync e2e tests that use it) working |

## 15. Open questions

- ~~The sidebar renders inside the vertical tab strip region, or as a separate
  view next to it.~~ Decided: inside the region, replacing only the native
  tab strip. See [docs/design/sidebar.md](docs/design/sidebar.md).
- What the placeholder in window A shows: a live thumbnail or a static card.
- ~~Whether archive is per space or global.~~ Per space. See
  [docs/design/sidebar.md](docs/design/sidebar.md), "Today auto-archive".
- How to update on Linux: rely on distro packages and Flatpak, or ship our own
  updater.
