# Sidebar: hosting and first slice

Status: decided. Resolves the first open question in ARCHITECTURE.md §15.
Based on Chromium 156.0.8078.48.

## What upstream already gives us

**Native vertical tabs.**
`chrome/browser/ui/views/frame/vertical_tab_strip_region_view.{h,cc}` is a
complete left-side region inside the browser frame:

- a top container (caption-button exclusion, new tab, collapse toggle)
- a content area (a vertical `FlexLayoutView`) that holds the native
  `TabStripView`
- a bottom container
- a resize handle, collapse/expand animation, expand-on-hover and drop targets

The profile pref `vertical_tabs.enabled` (`prefs::kVerticalTabsEnabled`,
registered in `chrome/browser/ui/tabs/tab_strip_prefs.cc`, default `false`)
switches between horizontal and vertical layout. The native strip is built
from `TabStripModel`'s collection tree (`RootTabCollectionNode`) and is put
into the content area by `VerticalTabStripRegionView::AddTabStripView()`.

**WebUI inside the frame.** Chrome 156 is experimenting with WebUI toolbar
controls (`chrome/browser/ui/views/toolbar/webui_toolbar_web_view.*`,
`chrome/browser/ui/webui/webui_toolbar/`, `chrome://webui-toolbar.top-chrome`).
They're built on the "Initial WebUI" infrastructure in
`chrome/browser/ui/webui/top_chrome/`: a preloaded renderer process,
resources served from memory, and compositor surface sync so the WebUI paints
in the same frame as the native UI. This is the same approach as our plan
(ARCHITECTURE.md §4, D2), and the infrastructure is reusable.

## Decision

**The sidebar is a WebUI (`chrome://compass-sidebar`)
hosted inside the content area of `VerticalTabStripRegionView`. It replaces
only the native `TabStripView`. Everything else in the region stays upstream.**

- **We keep for free:** the frame and caption-button layout, resizing,
  collapse animation, expand-on-hover, and the top container. The bottom
  container only holds upstream's New Tab button, which the page's New Tab
  row replaces, so `InstallSidebar()` hides it.
- **The native `TabStripView` isn't built** (since slice e). Until then it
  was hidden in place, because the region's drag, hover-card and resize code
  assumed it. See "Slice e".
- **Compass always uses vertical mode.** The pref default becomes `true`,
  `VerticalTabStripStateControllerImpl` sets it back to `true` whenever
  something writes `false`, and `IDC_TOGGLE_VERTICAL_TABS` is a removed
  command (`compass::IsRemovedCommand()`), left out of the app menu, the
  macOS View menu and the title bar menu (D20).

Alternatives we rejected:

- **A separate view next to the content.** We'd duplicate the frame
  integration upstream already solved: caption buttons, resizing, animation,
  RTL.
- **Restyling the native `TabStripView`.** It can't represent nested folders
  or unloaded pinned entries, and isn't written in web tech (D2, D5).

## Hooks into upstream

Each of these is the smallest possible change. Overrides can't rename a
virtual method like `AddTabStripView()` in one translation unit without
breaking the vtable, so this case uses a patch. The full list with reasons is
in [patches/README.md](../../patches/README.md).

| Change | Patch |
|---|---|
| Put the sidebar where the native tab strip would be | `vertical_tab_strip_region_view.{h,cc}`: `InitializeTabStrip()` and friends (slice e) |
| Vertical tabs on by default | `tab_strip_prefs.cc`: 1 line |
| Vertical tabs only | `vertical_tab_strip_state_controller_impl.cc`, plus the menus that offer the toggle (D20) |
| Register `chrome://compass-sidebar` | `chrome_web_ui_configs.cc`: one call to `compass::RegisterWebUIConfigs()` |
| Bind the sidebar's Mojo interfaces | `chrome_browser_interface_binders_webui_parts_desktop.cc`: one call to `compass::PopulateWebUIInterfaceBrokers()` |
| Link `//compass/browser/ui` into `//chrome/browser/ui` | `chrome/browser/ui/BUILD.gn`: dep plus `allow_circular_includes_from`, same as `tab_search` |
| Ship the resource pak | `chrome/chrome_paks.gni`: 2 lines |
| Allow `//compass/*` to use Lit | `third_party/lit/v3_0/BUILD.gn`: visibility list |
| Reserve resource IDs | `tools/gritsettings/resource_ids.spec`: one entry |

## Hosting in slice a

For now the sidebar is a plain `views::WebView` loading `chrome://compass-sidebar`
(a `MojoWebUIController`), not a preloaded top-chrome WebUI. The top-chrome
path needs a `.top-chrome` host and an entry in the WebUI-name allowlist
generated from `tools/metrics/histograms/metadata/page/histograms.xml`. That's
one more patch, so it's deferred until first-paint latency matters. The page
finds its window through the upstream `webui::SetBrowserWindowInterface()` /
`GetBrowserWindowInterface()` embedding-context API.

## Code layout (new, in `//compass`)

```
browser/ui/views/sidebar/compass_sidebar_view.{h,cc}   Views host (views::WebView) + InstallSidebar()
browser/ui/webui/sidebar/compass_sidebar_ui.{h,cc}     MojoWebUIController + config
browser/ui/webui/compass_web_ui_registry.{h,cc}        Config and Mojo binder registration hooks
browser/ui/webui/sidebar/sidebar.mojom                PageHandlerFactory / PageHandler / Page
browser/ui/webui/sidebar/sidebar_page_handler.{h,cc}  Bridges the browser to the page
resources/sidebar/                                    Lit + TypeScript UI, built with build_webui()
```

## Slice a: read-only sidebar

Goal: the sidebar lists the window's open tabs and clicking one activates it.
`SpaceModel` isn't involved yet. That's slice b.

Mojo interface (first version):

```
interface PageHandler {
  GetTabs() => (array<Tab> tabs);
  ActivateTab(int32 tab_id);
  CloseTab(int32 tab_id);
  NewTab();
};
interface Page {
  TabsChanged(array<Tab> tabs);      // Whole list, small and simple. Diffs come with slice b.
  ActiveTabChanged(int32 tab_id);
};
struct Tab { int32 id; string title; url.mojom.Url url; url.mojom.Url favicon_url; bool active; };
```

`SidebarPageHandler` observes the window's `TabStripModel`
(`TabStripModelObserver`) and the tabs' title and favicon changes.

**Done when:** in a debug build, a new window shows the WebUI sidebar on the
left with every open tab. Opening, closing and switching tabs updates it
within one frame. Resizing and collapse still work.

**Tests:** a browser test checking that the sidebar lists tabs and that
activation works, plus unit tests for the page handler with a
`TestTabStripModelDelegate`.

## Slice b: SpaceModel is the source of truth

Goal: the sidebar renders the `SpaceModel` tree instead of the tab strip.
The tab strip becomes a mirror of the live tabs.

**Ownership.** `compass::WindowFeatures` (`browser/window_sync/`) owns one
`SpaceModel` and one `TabStripBridge` per window. It's a member of upstream's
`EmbedderBrowserWindowFeatures`, the per-window slot Chromium reserves for
embedders. That takes two small patches: a member in the header, and creation
in `Init()`.

The model is **per window** for now. ARCHITECTURE.md §7 shares one model
across all windows of a space. Without move-on-focus, though, a tab that's
live in another window couldn't be activated from this one. Moving the model
up to the profile, together with `LiveTabRegistry`, is a window-sync change.
The bridge already takes a `SpaceModel*`, so only the owner changes.

**`TabStripBridge`** maps `tabs::TabHandle` ↔ node ID for the live tabs of
one window. It keeps the two sides in line:

| Strip event | Model change |
|---|---|
| Tab inserted | New Today tab, right after the Today entry that holds the tab before it (or first) |
| Tab moved, and its node is a direct child of Today | Same move in Today |
| Tab moved, node elsewhere (a split, later pinned) | None. The strip is put back. |
| Tab removed | Node removed (a split left with one pane dissolves) |
| Title or URL changed | `SetTitle` / `SetUrl` |

| Model event | Strip change |
|---|---|
| Node moved | Live tabs reordered to match the tree (depth-first) |
| Node removed | Its live tabs are closed |

After every change, the live tabs in the strip are in the tree's depth-first
order. Extension tab indices stay meaningful (ARCHITECTURE.md §5). A
re-entrancy guard stops the bridge from reacting to its own changes.

**Closing from the sidebar** goes through the strip (`CloseWebContentsAt`),
so `beforeunload` can still cancel it. The node is removed only when the tab
is actually gone. Closing an entry without a live tab removes it from the
model.

**Mojo.** The page gets the tree once, then diffs. All of them come through
`Page`, so they arrive in order. A `GetTree()` reply would come on the other
pipe and could pass a diff.

```
RequestTree()  →  SetTree(Tree tree)      // pinned and today roots, active node
NodeAdded(parent_id, index, Node node)
NodeMoved(id, new_parent_id, index)
NodeChanged(id, title, url)
NodeRemoved(id)
ActiveNodeChanged(id?)
```

Node IDs are the model's UUIDs as strings. Which node is active is per window,
so it stays out of the model.

**Not in b:** upstream split tabs and tab groups aren't mirrored yet. Upstream
pinned tabs aren't either, and the hidden native strip is the only way to
create them. Splits arrive with §9, pinning with slice c.

**Tests:** bridge unit tests (both directions, guard, splits dissolving) on
`BrowserWithTestWindowTest`, page handler tests for the diffs, and the
existing browser tests.

## Slice c: pinned section, folders, drag and drop

Goal: pin tabs, nest them in folders, and rearrange everything by dragging.
Persistence is slice d, so pinned entries last until the window closes.

**Pinned entries outlive their tabs.** A pinned tab node can be *live* (it
has a tab in the strip) or *unloaded*.

| Action | Today entry | Pinned entry |
|---|---|---|
| Tab closes (×, ⌘W, page closes itself) | Node removed | Node stays, unloaded |
| Click | Activate | Activate, or open a tab for it if unloaded |
| × on an unloaded entry | (none) | Node removed |
| Drag Today → Pinned / Pinned → Today | Pin / unpin; the tab stays live | |

`TabStripBridge::OpenNode()` opens a tab for an unloaded node. It navigates
to the node's URL at the node's depth-first position in the strip. The
bridge *adopts* the inserted tab for that node instead of creating a Today
entry. A pinned tab that navigates updates its node, as in Arc. "Back to
pinned URL" comes later.

**Strip order.** Live pinned tabs come first, then Today, depth first, as
before. Upstream's own pinned-tab flag isn't used. After every strip-driven
insert or move, the bridge schedules a re-sync, so a tab dropped before the
pinned block is put back after it.

**Folders.** Only in Pinned (the model already enforces this). A "New
folder" button sits in the Pinned header. Double-click renames. Clicking a
folder collapses or expands it (see "Folder collapse state"). Deleting a folder removes
its subtree and closes its live tabs.

**Drag and drop** happens in the page, with HTML5 drag events on the rows:

- A drag that starts during a scroll, or within 350 ms of its last wheel
  event, is cancelled: a finger that presses the trackpad while two fingers
  scroll makes a press that moves, which would otherwise pick up the row
  under the pointer (`startsDrag_()`).
- On a tab row, the top half drops before it and the bottom half after it.
  On a folder row, the middle third drops into it.
- Section bodies take drops at the end, and the empty Pinned section shows a
  drop hint.
- The page sends `MoveNode(id, new_parent_id, index)`. `index` is the slot
  among `new_parent`'s *current* children, so the page doesn't need to know
  how `SpaceModel::Move()` counts. The handler converts it, checks
  `CanMove()`, and ignores invalid drops.

Mojo additions:

```
MoveNode(id, new_parent_id, index)
AddFolder(parent_id, index, title)
RenameNode(id, title)
DeleteNode(id)          // folders and pinned entries; closes live tabs
```

`ActivateNode()` on an unloaded tab node opens it. Whether a node is live is
window state, kept by the bridge, not the model. `Node` gains `live`, and
`Page.NodeLiveChanged(id, live)` reports changes, so unloaded entries can be
dimmed. The bridge reports them through its own `TabStripBridge::Observer`.

**Not in c:** creating splits by dragging (§9), dragging between windows,
and context menus (they came later; see "Context menus").

## Slice d: one model per profile, persisted

Goal: pinned entries and folders survive restarts and are the same in every
window.

**The model moves to the profile.** Persisting per-window models would give
two windows two diverging copies of Pinned. So `compass::SpaceService`
(`browser/window_sync/`) is attached to the `Profile` as user data, which
needs no patch. It owns:

- the profile's one `SpaceModel` (spaces come with ARCHITECTURE.md §6);
- the `LiveTabRegistry`, which maps node ↔ `TabHandle` for every live tab
  in every window and tells observers when a node gains or loses its tab;
- the `SidebarStore`, which persists Pinned (and Today, see "Today
  persistence").

Each window keeps a `TabStripBridge`, now for *its own* live tabs only. Strip
order is the depth-first order of the nodes live in that window. Today
shows the open tabs of every window, as ARCHITECTURE.md §7 describes.

**Move on focus** is the part of window sync this needs. Activating a node
whose tab lives in another window moves the tab here. It uses
`DetachTabAtForInsertion()` and `InsertDetachedTabAt()`, the same path as
dragging a tab between windows, so page state, media and forms survive.
The removal there has a reason for which `WillDeleteTab()` is false, so the
other window's bridge keeps the node. This window's bridge finds the tab in
the registry and keeps its node. If the other window had only that tab,
upstream closes it. A placeholder and the parking `Browser` come with full
window sync.

A tab that really closes still removes its Today node, or unloads its
pinned node. Closing a window therefore drops its Today tabs from the
sidebar, which matches today's behavior. The parking `Browser` will change
that.

**Persistence: JSON through `ImportantFileWriter`,** not SQLite (this changes
ARCHITECTURE.md §5; see the decision log). Pinned is a small tree that's
cheapest to write whole. `ImportantFileWriter` gives atomic writes,
coalescing and a background sequence, and it's how upstream stores
bookmarks, a similar tree. SQLite starts paying off with archive and
history, so it can come in then.

- File: `<profile>/Compass Sidebar.json`. It holds
  `{"version": 1, "pinned": [<node>...]}`, where a node is
  `{"type": "tab"|"folder"|"split", "title", "url", "children"}`.
- Load: read on a background sequence when the service is created, then fill
  Pinned. Saved entries start unloaded. Changes made before the load finishes
  are kept, and loaded entries are inserted ahead of them.
- Save: any change to Pinned schedules a write, coalesced over 1 s, and
  closing a window flushes. Changes that only touch Today are ignored.
  Logging out of macOS ends the session without closing windows or
  destroying the profile, so the interval is kept short.
- Off-the-record profiles get a model but no store.
- Node IDs aren't stored. They're only stable for a session.

Today came later; see "Today persistence". Folder collapse state came
later too; see "Folder collapse state".

**Tests:** a serializer round trip (pure, in `browser/sidebar/`), a store
test (save, then load into a new model), and bridge tests with two windows:
shared Today, move on focus, and a tab closing in one window while the
other window's page sees it.

## Window sync: closing windows, fallback, placeholder

Goal: windows are views onto the space, as ARCHITECTURE.md §7 says.
Closing a window doesn't close its tabs, and moving a tab away never closes a
window.

**Closing a window hands its tabs to another window** of the profile: the
most recently active one that isn't closing. They're moved, not reloaded,
and they keep their nodes. That window's bridge puts them in tree order,
in the background. This replaces the hidden parking `Browser` (D8, now
D13). A hidden window brings its own problems: session restore would bring
it back visible, and it keeps the process alive with no UI on Windows and
Linux. Handing off to a real window gives extensions a `windowId` just
the same. When spaces arrive (one `Browser` per window and space), the
hidden `Browser`s of inactive spaces will hold such tabs anyway.

- Hook: `BrowserView::OnWindowCloseRequested()`, before `beforeunload`
  runs, because the tabs aren't closing. One call to
  `compass::HandOffTabsBeforeClose()`. If it moved the tabs, the request
  returns `kCannotClose`, and the now empty strip closes the window through
  upstream's `TabStripEmpty()` path, the same as dragging out the last tab.
- No hand-off when quitting (`browser_shutdown::IsTryingToQuit()`), for
  non-normal windows, or for the last window of the profile. There the tabs
  close, and their Today entries stay, unloaded (see "Today persistence").

**Move on focus, the source side.** A window never switches away from the
tab it shows. When that tab leaves for another window, a placeholder takes
its place in the strip and stays active, even if the window has other tabs.
For moves through the sidebar, the bridge inserts the placeholder before the
tab leaves. Other moves, like upstream's "Move tab to new window" or an
extension, are caught in the strip's removal notification: the screenshot
request goes out right away, and the placeholder follows in a task. A tab
that wasn't shown leaves no placeholder.

**Placeholder:** `chrome://compass-placeholder`, a small WebUI in the same
resource pak as the sidebar. It shows a **frozen screenshot** of the tab,
taken with `CopyFromSurface()` just before the tab left. The screenshot is
JPEG-encoded off the UI thread and kept by the bridge until the placeholder
closes. A pill on top says "Open in another window" and has a **Bring here**
button. If there's no screenshot, it shows the title and the button.

**Back in focus, the tab comes back.** When a window becomes active while its
active tab is a placeholder, the bridge brings the tab back automatically.
That's move on focus from both sides: two windows side by side hand the tab
to whichever one you use. The bridge tracks placeholder tab → node and
doesn't mirror placeholders into the model. The sidebar shows the node as
active while its placeholder is. Placeholders close when:

- the node comes back to this window (Bring here, or a sidebar click);
- the window activates any other tab;
- the node is removed.

Mojo: `PlaceholderHandler { GetInfo() => (title, url); GetSnapshot() =>
(data_url); BringHere(); }`. `GetSnapshot()` answers once the encode is done,
with an empty string if the capture failed.

**Tests:** bridge tests for the fallback, the placeholder lifecycle and the
hand-off (two windows), and a browser test that closes a real window and
checks its tab lives on in the other one.

## Today persistence

Goal: Today survives closing the last window and restarting, as
ARCHITECTURE.md §7 describes. Entries come back unloaded and load when
clicked.

**Saved with Pinned.** The sidebar file gains a `"today"` list, next to
`"pinned"`, in the same node format (tabs and splits). The version stays 1:
older files have no `"today"` and load as before. Every model change
schedules a write, still coalesced over 1 s; browsing changes titles and
URLs often, but the file is small. Today tabs that aren't worth restoring,
new tab and blank pages (`IsRestorableURL()`), are left out, so each launch's
new tab page doesn't pile up.

**Window closing unloads Today.** A tab that closes on its own still removes
its Today entry. A tab that closes because its window does
(`TabStripModel::closing_all()`, normal windows) keeps it, unloaded, like a
pinned entry. That covers the last window of the profile and quitting; other
windows hand their tabs off first. Restorable URLs only, as above.

**Tabs Chrome restores take over their entries.** Chrome's own session
restore ("Continue where you left off", crash recovery) and reopening a
closed tab bring back tabs with their history. Without help, each would
become a new Today entry next to its saved, unloaded one. A tab whose last
committed entry `IsRestored()` takes over the first unloaded entry with its
URL, pinned first, depth first (`RestoredTabMatcher`). The file loads in the
background and may arrive after restore has started, so tabs restored before
the load get their own Today entry first. Once the load is in, each moves to
its saved entry and the extra entry goes. The strip then follows the tree's
order again. The URL is the restored entry's while the tab hasn't loaded,
since it has no document yet.

URLs aren't unique, so two saved entries with the same URL can swap tabs.
Both have the same page, so it shows only in history. Tagging tabs with node
IDs through session "extra data" would be exact, but it needs two patches:
upstream drops tab extra data when it rebuilds the session file (D14).

**Tests:** serializer (Today saved, new tab and blank pages left out, folders
in Today skipped), store (Today round trip), and bridge tests: closing the
window unloads Today, a restored tab takes over its entry while an ordinary
one doesn't, and tabs restored before the load move to their entries when it
arrives (a second profile with a sidebar file written first).

## Slice e: no native tab strip

Goal: the sidebar replaces the native strip instead of sitting next to a
hidden one. Nothing builds tab views for tabs nobody sees, and nothing native
handles input meant for the sidebar.

**Hook.** Upstream builds the native strip in
`BaseTabStripRegionView::InitializeTabStrip()`: a `RootTabCollectionNode`
tree with a view per tab, the `TabDragHandler` and the
`TabHoverCardController`. The vertical region now overrides
`InitializeTabStrip()` and `ResetTabStrip()`. It creates the sidebar
(`compass::CreateSidebar()`) and passes it to its own `AddTabStripView()`, so
the sidebar lands where the strip would. The region then subscribes to the
collapse animation and expand on hover as before.
`compass::InstallSidebar()` then replaces the strip's flex rule and margins.
`BrowserView` calls these overrides when it switches between vertical and
horizontal, so the horizontal strip still works if someone switches back.

**What assumed the native strip, and what happens now:**

| Path | Now |
|---|---|
| Resize handle (`OnResize`) `CHECK`ed the strip | Skips the strip's animating flag |
| Collapse animation started only once the strip existed | Starts once the region subscribed to it |
| Hover-card hide lock while animating or expanding on hover | Only if there's a hover-card controller (there isn't) |
| `GetTabData()` for accessible tab and window titles read the tab view | `TabData::FromTabInterface()`, from the tab itself |
| Active-tab changes came through the node tree | `compass::ActiveTabWatcher`: the window's active-tab callback, plus null as soon as that tab leaves. A closing window closes its tabs without changing the active one, and the region observes the active tab's `OmniboxTabHelper`, so without that it kept a destroyed helper (a use-after-free at window close). The omnibox popup still keeps an expanded-on-hover region collapsed |
| Native tab drags (`GetTabDragTarget()`) | No drag handler, so no native drag targets; there are no native tabs to drag |
| Link drops: the region was the drop target while the strip existed | The sidebar page takes them (below) |

Everything else in `BaseTabStripRegionView` already handled a missing node
tree (`GetTabAnchorView()` returns null, for example).

**Link drops.** A link dragged in from a page or another app opens as a new
entry where it's dropped: before or after a row, into a folder, or at the
end of a section (`PageHandler.OpenLink(url, parent_id, index)`, http(s) and
file URLs only). The entry opens in the foreground. The document takes every
drag that no drop target took, with `dropEffect = 'none'`, so nothing
navigates the sidebar's own page.

**Not taken over yet:** dragging an entry out of the sidebar into a new
window. Hover cards and keyboard navigation came later (see their sections).

**Tests:** browser tests that the region has no `TabStripView` and a sized
sidebar, that resizing and collapsing work, and that a dropped link opens
in Pinned without navigating the sidebar.

## Today auto-archive

Goal: Today doesn't fill up with entries nobody looks at, as in Arc. An
entry that goes unseen for its space's delay (12 hours by default) moves to
the space's archive, from where one click brings it back.

**Last active.** Each tab node has a last-active time, saved with it in the
sidebar file. A window's bridge stamps the entry it shows when it starts
showing it and again when it stops (the strip's active-tab change), so the
clock starts when the entry leaves the screen. A tab with no time yet (opened
in the background, or loaded from an older file) gets one the first time the
archiver sees it, so it gets a full delay too. A split counts from its most
recently shown pane.

**The archiver.** `SpaceModel::ArchiveIdleEntries(now, keep)` archives every
Today entry whose time is older than the space's delay, except those `keep`
spares. Pinned entries are never archived. `SpaceService` runs it for every
space 30 s after the file loads, so restored tabs have taken over their
entries, and then every 10 minutes. It keeps entries that a window shows
(a placeholder counts), and entries with a tab playing sound.

**Archiving** records the entry's tabs (title and URL; a split keeps its
panes) with the time, then removes the entry, which closes its live tabs
with history, so ⌘⇧T still works. New tab and blank pages aren't recorded.
The archive is per space (this settles the open question in
ARCHITECTURE.md §15), newest first, and keeps the last 100 entries. It's
saved in the sidebar file as the space's `"archive"`. It isn't searchable
yet; that wants SQLite (D12).

**Restoring** puts the entry back at the end of Today, unloaded, and opens it
in the foreground (a split opens its first pane).

**UI.** Today gets a header with an archive button. It opens a panel over the
sections with the archived entries, their age, a × to drop one and Clear.
The space menu gets "Archive Today tabs after": 12 hours, 24 hours, 7 days,
30 days or never. The file stores it as `"archive_after_hours"` (0 for never).

Mojo: `Space.archive_after_hours`; `PageHandler.SetArchiveAfter(space_id,
hours)`, `RestoreArchived(id)`, `RemoveArchived(id)`, `ClearArchive()`;
`Page.SetArchive(entries)`, sent after the tree and on every change.

**Tests:** model (idle entries, splits, never, the cap, restoring),
serializer (last active, delay and archive round trip; older files get the
default), a bridge test (unseen tabs close and archive, the shown one stays)
and a page handler test.

## Browser UI, not a page

The sidebar's page drops what a web page would do but the browser's own UI
doesn't:

- No zoom. `CompassSidebarView::PreHandleGestureEvent()` drops pinches and
  smart zoom (a two-finger double tap) before the page sees them; the page
  cancels Ctrl+wheel, which a pinch also arrives as. `CompassSidebarUI` is a
  `TopChromeWebUIController`, so the page stays at 100% whatever the default
  page zoom.
- No text selection or text cursor (`app.css`), except in the name fields
  being edited, which have no spellcheck or autocomplete either.
- No horizontal rubber band or scroll chaining (`overscroll-behavior-x:
  none`), so a horizontal swipe is the space switcher's, never back or
  forward. Lists still rubber-band vertically at their ends, as native lists
  do (`contain`: no chaining to the list around them).
- Favicons can't be dragged out alone: a drag from one moves its row.
- No native context menu outside the name fields, and drops outside the
  drop targets do nothing (both already in `app.ts`).

## Keyboard navigation

Goal: the sidebar's entries can be used without a mouse.

The page handles it alone. Tab rows (panes too) and folder rows take the
focus (`tabindex="-1"`), and the list of sections is the one tab stop:
tabbing into it lands on the active entry, or the first one. While a row has
the focus, the list leaves the tab order, so Shift+Tab leaves the sidebar
instead of landing back on it.

| Key | Does |
|---|---|
| ↑ ↓, Home, End | Moves between rows as they show (collapsed folders' children are skipped) |
| → | Expands a collapsed folder, or goes to its first child |
| ← | Collapses an expanded folder, or goes to the parent folder |
| Enter, Space | Activates a tab (the focus stays in the sidebar), or toggles a folder |
| Delete, Backspace | Closes a tab, as its × does; the focus moves to the next row |
| F2 | Renames a folder; Enter or Escape gives the focus back to its row |

Focus shows with `:focus-visible`, so clicks don't draw a ring. There's no
shortcut that moves the focus from the page to the sidebar yet.

Favorites tiles come before these rows; see command-bar.md, "Slice 4",
"Keyboard".

## Hover cards

Goal: resting the pointer on a tab shows upstream's hover card next to the
sidebar: the full title, the domain, and a preview of a live tab.

**Upstream's bubble, our anchor.** `TabHoverCardBubbleView` takes any
`HoverCardAnchorTarget`, so the native card is reused without the native
strip. `SidebarHoverCard` (`browser/ui/views/sidebar/`) is user data on the
sidebar page's WebContents. It owns the bubble and an empty anchor view, a
child of `CompassSidebarView` as wide as the sidebar and placed over the
hovered row, so the card opens to the sidebar's right (`LEFT_TOP`). Moving
the anchor moves the card. Upstream's `TabHoverCardController` isn't used: it
needs a `TabStripRegionView` and native tab views.

**Data.** A live tab's card comes from `tabs::TabData::FromTabInterface()`,
as upstream's does, wherever the tab lives. An unloaded entry gets its title
and URL. A live tab that isn't the window's active one gets a preview,
through upstream's `TabHoverCardThumbnailObserver`; a placeholder shows until
it arrives. A tab gets its image when it goes to the background; one that was
never shown is captured in the background while its card is up, which takes
a few seconds (much longer in a debug build). As upstream's controller does,
a new card gets its content before it shows, and it's sized to its content
each time. Previews follow upstream's rule: the `kTabHoverCardImages`
feature and the user's setting (Settings > Appearance). Without the feature,
tabs have no `ThumbnailTabHelper`, so there's nothing to show. Upstream turns
the feature off on macOS only; Compass turns it on everywhere (one-line patch
to `ui_features.cc`).

**The page decides when.** It sends `ShowHoverCard(id, row_top, row_height)`
after the pointer rests on a tab for 500 ms (upstream's delay), and right
away while a card is up, so moving down the list moves the card. Crossing
the gap between rows doesn't hide it. Leaving the list, pressing, dragging,
scrolling or the row going away sends `HideHoverCard()`. A pressed tab
doesn't show its card again until the pointer moves to another tab. Folders
and splits as a whole have no card; panes do. The card can't take focus or
activate the window.

**Not done:** cards for keyboard focus, upstream's slide and fade
animations, and the memory usage footer.

**Tests:** a browser test drives the page's hover handling and checks the
card shows, moves and hides. The keyboard handling is checked by driving the
page in a browser test too.

## Context menus

Goal: right-clicking an entry offers what can be done with it, as in Arc,
without dragging or hunting for buttons.

**Native menus, built by the browser.** The page only says where it was
right-clicked: `PageHandler.ShowContextMenu(id, x, y)`, for the row or tile
under the pointer (a pane's own ID inside a split), in page coordinates.
`SidebarPageHandler::CreateContextMenu()` builds a `ui::SimpleMenuModel` for
the node, and `SidebarContextMenu` (`browser/ui/views/sidebar/`, user data on
the sidebar page's WebContents, like the hover card) runs it with upstream's
`views::MenuRunner` at that point, converted to the screen. A native menu
isn't clipped by the narrow sidebar, looks like the platform's and handles the
keyboard and accessibility itself. The menu's commands go back to the handler
by node ID, so a node that went away while the menu was up is left alone, and
so is everything once the handler is gone.

The page's own menu (Back, Reload, Inspect) never shows in the sidebar; text
fields (a folder's or space's name) keep theirs. The context menu key opens
an entry's menu under its row.

| Entry | Menu |
|---|---|
| Today tab | Copy Link, Duplicate, Reload, Mute Tab · Pin, Add to Favorites, Move to Space ▸ · Close Tab |
| Pinned tab | Copy Link, Duplicate, Reload, Mute Tab · Unpin, Add to Favorites, Move to Space ▸ · Close Tab, Remove from Pinned |
| Favorite | Copy Link, Duplicate, Reload, Mute Tab · Close Tab, Remove from Favorites |
| Folder | New Folder, Rename, Move to Space ▸ · Delete Folder |
| Split | Pin or Unpin, Move to Space ▸ · Close Split |
| Pane | Copy Link, Reload, Mute Tab · Close Tab |

- **Reload**, **Mute Tab** and **Close Tab** only for live tabs; a muted
  tab gets **Unmute Tab** instead (see "Sound"). An unloaded Today entry
  gets **Remove** instead, as its × does.
- **Pin** adds to the end of Pinned; **Unpin** puts the entry at the top of
  Today, which keeps its tab where it was in the strip.
- **Move to Space** lists the other spaces and is left out with only one. It
  moves as dragging onto a space button does (`MoveEntryToSpace()`).
- **Duplicate** copies a tab that's live in this window with its history
  (`chrome::DuplicateTabAt()`); the bridge puts the copy's Today entry after
  it. Otherwise it opens a new Today entry for the URL, after the entry in
  Today, or at the top, in the foreground.
- **Rename** asks the page to edit the folder's name in place
  (`Page.StartRename(id)`), as a double-click does. **New Folder** adds
  "New Folder" at the end of the folder.

Labels are English literals for now, like the rest of Compass's UI; they move
to a grit file when the UI is localized.

**Not yet:** menus for the section headers and empty space (New Folder, New
Tab), Copy Link of a split, and opening in a new window (which needs
drag-out's window creation).

**Tests:** page handler tests build each kind of menu and choose its
commands (pin, unpin, remove, favorites, move to space, duplicate live and
unloaded, copy link, folder commands, a node gone before the choice). A
browser test right-clicks rows in the page and checks the menu, its
position and Rename (`SidebarContextMenu::SetShowCallbackForTesting()`
hands the model over instead of running a native menu, which would block the
test).

## Sound

Goal: as in Arc, a tab that plays sound shows a speaker on its row, and
clicking the speaker mutes it. A tab that isn't playing is muted ahead of
time from its context menu.

- `LiveTabRegistry` follows every live tab's sound, profile-wide, so a
  tab's speaker shows in every window of its space: whether its contents
  are muted (`WebContentsObserver::DidUpdateAudioMutingState()`), and
  whether they played sound in the last two seconds (upstream's
  `RecentlyAudibleHelper`, the same signal as the tab strip's alert icon,
  so the speaker doesn't flicker between sounds). It follows the contents a
  discarded tab swaps to. `Observer::OnNodeAudioChanged()` reports changes.
- `Node.audio` (`kNone`, `kAudible`, `kMuted`) carries it in the tree, and
  `Page.NodeAudioChanged()` sends changes. Muted wins over playing: a muted
  tab shows the crossed-out speaker whether or not it plays, so a tab muted
  from the menu shows it can be unmuted.
- The speaker sits after the title on tab rows and panes, and in a
  favorite tile's top-left corner (the × is top-right). Clicking it sends
  `PageHandler.SetNodeMuted(id, muted)`, which mutes through upstream's
  `SetTabAudioMuted()` with `TabMutedReason::kAudioIndicator`, as the tab
  strip's icon does.
- **Mute Tab** / **Unmute Tab** in a live tab's context menu do the same.
- **Cmd+Shift+M** (`kToggleTabMuteCommand`, shortcuts.md) mutes or unmutes
  the window's active tab; it can be changed on the shortcuts settings page.

### Muted entries

As in Arc (`savedMuteStatus`), muting belongs to the entry, not only to its
live tab: a muted pinned tab or favorite closed and opened again, or a
muted Today entry after a relaunch, comes back muted.

- `SidebarNode::muted()` (tabs only), set with `SpaceModel::SetMuted()`,
  which notifies `OnNodeChanged()`. The store saves it on the tab
  (`"muted": true`; absent means not muted, so older files load as before).
- It follows the live tab, however that was muted (speaker, menu,
  Cmd+Shift+M, an extension, upstream's UI): `LiveTabRegistry` reports
  `Observer::OnNodeMutingChanged()` from the contents'
  `DidUpdateAudioMutingState()`, and `SpaceService` sets the node's flag in
  a posted task (the call can come in the middle of the registry's or a
  model's notifications, which don't allow nesting). A muted tab that gains
  an entry mutes the entry. Closing a tab leaves the flag as it is.
- `LiveTabRegistry::Register()` mutes a muted entry's tab before it tells
  anyone the entry is live, and mutes the contents a discard swaps in. Its
  own muting isn't reported back.
- An unloaded muted entry shows no speaker; it shows the crossed-out one
  once it opens.
- Not synced to other devices (the spaces sync proto has no field for it);
  the sync bridge keeps it when it copies a node locally.
- Tests: `SidebarSerializerTest.RoundTripsMutedTabs`,
  `SidebarPageHandlerTest.AMutedEntryReopensMuted`, and the Arc import's
  `SidebarImportTest.MutedTabsStayMuted`.

## Now playing

Goal: as in Arc, a tab that plays media (or paused it) and isn't on screen
gets a "now playing" widget at the bottom of the sidebar, above the space
switcher: the tab's icon, previous, play/pause, next and a speaker. Several
tabs, several widgets, stacked. Its title row (the media's title, a
picture-in-picture button and ×) shows when the pointer is on it, and on
every widget when there are several, to tell them apart.

- `NowPlayingTracker` (`browser/ui/webui/sidebar/`), one per sidebar page,
  finds media sessions through the audio focus manager (every session that
  ever requests focus, so a tab is watched only once it plays) and follows
  each with a `MediaSessionObserver` (state, title, actions), a
  `WebContentsObserver` (muting, title) and its tab's visibility and
  insert/detach callbacks, until its contents go. Not until its focus
  request is released: pausing from the widget abandons focus, and the
  paused widget must stay. Updates are coalesced in a posted task.
- A session shows in a window if it's the profile's, controllable, not
  inactive, not in picture-in-picture, and its tab is in the window but not
  visible (`TabInterface::IsVisible()`, so a split's shown pane counts as
  shown), or parked (another space's tab, or one a profile swap left). Parked
  tabs show in every window of the profile.
- A session must stay eligible for `kShowDelay` (800 ms) before its widget
  shows, so leaving a tab whose video goes to auto picture-in-picture
  doesn't flash one. Widgets keep the order they first showed in.
- `Page.SetNowPlaying()` sends the widgets after the tree and on changes;
  `PageHandler.ActOnNowPlaying(id, action)` runs a button: play/pause,
  previous/next track and picture-in-picture through
  `content::MediaSession`; mute through `SetTabAudioMuted()` as the
  speakers do; the icon and the title show the tab (`RevealParkedTab()` for a
  parked one). Previous and next are dimmed unless the site handles them;
  the picture-in-picture button shows only when the session offers it.
- Muting a tab takes its players out of its media session (Chromium's
  rule), which goes inactive. Its widget stays, as the session last was,
  with the crossed-out speaker: only the speaker works then (play/pause,
  previous, next and picture-in-picture are dimmed, `can_play_pause`), and
  unmuting brings the players back.
- × pauses the media and hides its widget until it plays again.
- The page side is `compass-now-playing` (`now_playing.ts`).
- Tests: `NowPlayingBrowserTest.*`.

## Folder collapse state

Whether a folder is collapsed is part of the model, as in Arc: a
`SidebarNode::collapsed()` flag on folders, set with
`SpaceModel::SetCollapsed()`, which notifies `OnNodeChanged()`. So it is
the same in every window of the profile, and the store saves it with
Pinned (`"collapsed": true` on a folder; absent means expanded, so older
files load as before). A folder keeps it when it moves to another space,
and a swipe's preview of another space shows that space's folders as
they are.

The page reads `Node.collapsed` (sent in the tree and in `NodeChanged`).
Clicking a folder, or ← and → on it, flips the flag on the page's own node
at once, so the motion starts on that frame, then calls
`SetFolderCollapsed()`. The echo changes nothing there; other windows
animate the change when their `NodeChanged` arrives.

Per-window collapse state, which the slice c plan suggested, was dropped:
windows of a profile show the same tree, and one state per folder is what
Arc does.

**Tests:** model (`FoldersCollapse`), serializer round trip
(`RoundTripsFolderCollapseState`), handler (`CollapsesFolders`) and the
browser test `FolderCollapseIsShared` (clicking collapses it in the model;
a second window shows it collapsed; expanding in the model expands both
pages).

## Folders in Today

Today has folders too, as Arc does: "+" in Today's header adds one at the
top (where new entries go), and folders nest, drag, collapse, rename and
move between spaces as Pinned's do. `SpaceModel::CanContain()` lets a
folder go in either root; a folder's menu also has Pin or Unpin, which
moves it with its tabs.

- Tabs in a Today folder stay live and Today's: closing one removes its
  entry, as anywhere in Today. Deleting a Today folder closes its tabs.
- The strip follows the tree's order (`CollectLiveTabs()` walks folders);
  a tab moved in the strip that sits in a folder keeps its place, as a
  split's panes do.
- Auto-archive goes by entry, tab or split, inside folders too; folders
  aren't archived (`ArchiveIdleEntries()`).
- "Clear" in Today's header (shown while the pointer is on it, as Arc's)
  closes every Today tab, in folders and splits too, each as its close
  button would (`ClearToday()`; beforeunload can keep one), and removes the
  folders left without a tab. Pinned stays.
- A Today folder closes when its last entry leaves: closed, archived,
  moved out or pinned, and so on up its emptied parents. A new folder,
  empty, stays until something has been in it. Sync's apply is exempt
  (`SpaceModel::KeepEmptyFolders`; iOS: `applyingRemote`): another
  device's changes can empty a folder for a moment, and its own deletion
  comes along. Pinned folders stay, emptied or not.
- The store saves Today folders like Pinned's, empty ones too; older
  builds skip them.

**Tests:** `FoldersNestInPinnedAndToday`, `EmptiedTodayFoldersClose`,
`ArchivesEntriesInFoldersOneByOne`,
`RoundTripsTodayFolders`, `SidebarPageHandlerTest.FoldersInToday` and the
browser test `FoldersInToday`.

## Tab groups

Compass has folders, not tab groups. A group made upstream (an extension's
`chrome.tabs.group()`, a page's or menu's "Add to group", session restore
of an older profile) becomes a Today folder, and the group ends:
`TabStripBridge::ConvertGroups()`, half a second after the change (so an
extension can name the group first), makes a folder where the group's
first Today entry was, named after the group ("Group" if it has no name),
collapsed if the group was, moves the group's Today entries into it in
strip order (a split whole), and takes the tabs out of the group
(`RemoveFromGroup()`). Pinned entries in a group only leave the group.

Keeping groups alive next to folders would fight the strip's order:
upstream puts a tab moved into a group's range into the group. So
extensions see the group end; its color isn't kept (folders have none).

**Tests:** `TabStripBridgeTest.TabGroupsBecomeFolders`.

## Split tabs

A split entry (`SidebarNode::Type::kSplit`, two panes, saved since slice
c) is upstream's split view when it's open: the window shows both panes
side by side. The bridge keeps the two in step (`TabStripBridge`):

- Model to strip: after every strip sync, the upstream splits that aren't
  a model split end (`RemoveUnmirroredSplits()`), the strip follows the
  tree with each split moved whole (`MoveSplitTo()`, panes in the model's
  order), and each model split whose panes are both live here becomes an
  upstream split (`CreateMirroredSplits()`, `AddToNewSplit()`).
- Strip to model: a split made upstream (the split view button, "Open
  link in split view", an extension) splits the two entries in the model,
  where the first one was; a pane replaced upstream changes the model's
  split. A split ended upstream with both tabs still in the window
  separates the entries; one that ended because a tab closed doesn't, so
  a pinned split stays a split while unloaded. A tab moved out of its
  split in the strip leaves it in the model too.
- Opening a pane (clicking it, or the split's active pane coming back)
  opens the other pane in the background, so the split shows at once
  (`OpenNode()`).

The context menu has "Open in Split View" on an entry that can go next to
the window's active entry (it splits them and shows the split), and
"Separate Tabs" on a split or a pane (`SpaceModel::Unsplit()`).

Dragging a tab entry onto the middle half of another tab entry's row (not
favorites) splits them: dropped on the row's left half, the dragged tab
goes on the left, on the right half on the right (mirrored right to left).
The row shows that half highlighted. The top and bottom quarters still
drop before and after. The page calls `SplitNode(id, target_id, first)`,
which splits and shows the split.

Not yet: dragging onto the page itself, and the upstream split's layout
and ratio (not saved).

**Tests:** `UnsplitPutsThePanesInItsPlace`, the bridge's
`ModelSplitsSplitTheWindow`, `UpstreamSplitsSplitTheModel`,
`StripMovesOutOfASplitSeparateIt`, `ClosingAPinnedPaneKeepsItsSplit`, the
handler's `SplitsAndSeparatesFromTheMenu` and
`DroppingATabOnAnotherSplitsThem`, and the browser tests
`SplitsShowSideBySide` and `DropOnATabSplitsThem`.

## Link folders

Links opened in the background from a page in the sidebar (Cmd+click,
middle click, "Open Link in New Tab") gather with that page in a Today
folder, so a page's tabs stay together:

- The opener in Today, not in a folder: a folder named after it (its name
  or title, else its host) takes its place, holding the opener and then the
  link. A split opener goes in whole.
- The opener in a Today folder (made this way or by hand): the link joins
  that folder.
- The opener pinned or a favorite: it stays put; its links gather in a
  folder at the top of Today, named after it, which later links reuse
  while it's there (`TabStripBridge::link_folders_`, per window, not
  saved).
- Within the folder, a link goes after the one the opener opened before
  (the tab before it in the strip, which upstream already puts after the
  opener's other links), else right after the opener.

`TabStripBridge::GetLinkFolderPlace()` decides, when the strip inserts the
tab: not active, a `PAGE_TRANSITION_LINK` navigation, and an opener with a
node (upstream sets the opener for links). Foreground links (target=_blank,
Cmd+Shift+click), typed URLs, the command bar and restored tabs open as
before. The strip then follows the tree. There's no setting to turn it off
yet.

**Tests:** `TabStripBridgeTest.LinksGatherWithTheirOpenerInAFolder`,
`LinksFromPinnedEntriesGatherAtTheTopOfToday`,
`ForegroundAndTypedTabsDontMakeFolders`.

## Renaming tabs

As in Arc, a tab entry (pinned, Today or a split's pane) can be given a
name: double-click its row, press F2 on it, or choose Rename in its context
menu (not on favorites, whose tiles show no name). The name shows as an
input, selected; Enter or leaving it keeps it, Escape drops it. An empty
name, or the page's own title, removes the name.

The name is `SidebarNode::custom_title()`, next to `title()`, the page's
title, which the bridge keeps updating underneath. `display_title()` is
the name if there is one. `SpaceModel::SetCustomTitle()` notifies
`OnNodeChanged()`, so every window shows it, and the store saves it as
`"custom_title"` on the tab (also on panes). The page gets
`Node.customTitle` in the tree and in `NodeChanged`, and calls
`RenameTab(id, title)`. The command bar shows the name and matches both
it and the page title. Hover cards show the page's own title.

Not kept: an archived entry comes back with its page title only, and a
duplicated entry has no name.

**Tests:** `RoundTripsCustomTitles`, `SidebarPageHandlerTest.RenamesTabs`,
the menu tests, and the browser test `DoubleClickRenamesATab`.

## Motion

Entries move instead of jumping. Before the page applies a diff
(`NodeAdded`, `NodeMoved`, `NodeRemoved`) or collapses a folder, it measures
every tile and row (by node ID) and the sections' other parts (section
headers, the divider, hints and New Tab, by `data-flip`):
`prepareFlip_()`. On the next frame, after Lit and the
`<compass-sidebar-node>`s have rendered, `playFlip_()` measures again and
plays FLIP animations (Web Animations, 220 ms):

- Moved: a translate from the old position. Pinning or unpinning (context
  menu, drag) is a `NodeMoved` across sections, so the row glides from Today
  up into Pinned while the rows between make room. Lit's `repeat` makes a new
  element for a node that changes parent; the key is the node ID, so it
  doesn't matter.
- A row that becomes a tile or the other way (Add to Favorites): it grows
  out of the old position's center, fading in.
- New: fades in from 4 px above.
- Gone: a copy fades out where it was. Copies live in `#ghosts`, a host with
  its own shadow root (the app's style sheets adopted), so the app's queries
  and the browser tests' don't see their rows.

Diffs within one frame animate together. A change mid-animation starts from
where the entry got to (the first measurement includes transforms). Nothing
animates while spaces slide (spaces.md, "Swiping") or with
`prefers-reduced-motion`, which also turns off the CSS transitions (row
hover and active backgrounds, a slight press scale, the space menu and the
archive popping in).



- **b:** `SpaceModel` becomes the data source, using the
  `TabStripModel` ↔ `SpaceModel` bridge.
- **c:** pinned section, nested folders, drag and drop.
- **d:** persistence (Today followed separately).
- **e:** remove the hidden native strip (done; drag-out remains).

## Risks

- **Initial WebUI is experimental upstream** (`webui_toolbar` is an
  experiment). If it changes, the sidebar can fall back to a plain preloaded
  `WebUIContentsWrapper`, as tab search uses. That's slower to first paint but
  stable.
- **Running the region without its native strip** relies on upstream's
  null checks for a region whose strip isn't built yet. New upstream code that
  assumes the strip (`CHECK(root_node_)`, `GetTabData()`-style calls) will
  crash at startup or on first use, so each rebase must run the browser tests.
