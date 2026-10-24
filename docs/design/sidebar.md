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

**The sidebar is a WebUI (`chrome://strata-sidebar`)
hosted inside the content area of `VerticalTabStripRegionView`. It replaces
only the native `TabStripView`. Everything else in the region stays upstream.**

- **We keep for free:** the frame and caption-button layout, resizing,
  collapse animation, expand-on-hover, and the top and bottom containers.
- **We hide but don't remove the native `TabStripView`.** The region's drag
  handler, hover cards and resize code call `tab_strip_view()` and `CHECK` it.
  It stays in the hierarchy with `SetVisible(false)` until we take over those
  paths too. That's a later slice.
- **Strata always uses vertical mode.** The pref default becomes `true`.
  Hiding the horizontal/vertical toggle comes later.

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
| Install the sidebar when the region adds its tab strip | `vertical_tab_strip_region_view.cc`: one call to `strata::InstallSidebar()` |
| Vertical tabs on by default | `tab_strip_prefs.cc`: 1 line |
| Register `chrome://strata-sidebar` | `chrome_web_ui_configs.cc`: one call to `strata::RegisterWebUIConfigs()` |
| Bind the sidebar's Mojo interfaces | `chrome_browser_interface_binders_webui_parts_desktop.cc`: one call to `strata::PopulateWebUIInterfaceBrokers()` |
| Link `//strata/browser/ui` into `//chrome/browser/ui` | `chrome/browser/ui/BUILD.gn`: dep plus `allow_circular_includes_from`, same as `tab_search` |
| Ship the resource pak | `chrome/chrome_paks.gni`: 2 lines |
| Allow `//strata/*` to use Lit | `third_party/lit/v3_0/BUILD.gn`: visibility list |
| Reserve resource IDs | `tools/gritsettings/resource_ids.spec`: one entry |

## Hosting in slice a

For now the sidebar is a plain `views::WebView` loading `chrome://strata-sidebar`
(a `MojoWebUIController`), not a preloaded top-chrome WebUI. The top-chrome
path needs a `.top-chrome` host and an entry in the WebUI-name allowlist
generated from `tools/metrics/histograms/metadata/page/histograms.xml`. That's
one more patch, so it's deferred until first-paint latency matters. The page
finds its window through the upstream `webui::SetBrowserWindowInterface()` /
`GetBrowserWindowInterface()` embedding-context API.

## Code layout (new, in `//strata`)

```
browser/ui/views/sidebar/strata_sidebar_view.{h,cc}   Views host (views::WebView) + InstallSidebar()
browser/ui/webui/sidebar/strata_sidebar_ui.{h,cc}     MojoWebUIController + config
browser/ui/webui/strata_web_ui_registry.{h,cc}        Config and Mojo binder registration hooks
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

**Ownership.** `strata::WindowFeatures` (`browser/window_sync/`) owns one
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
folder collapses or expands it. Collapse state lives in the page for now and
becomes per-window state with persistence (slice d). Deleting a folder removes
its subtree and closes its live tabs.

**Drag and drop** happens in the page, with HTML5 drag events on the rows:

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
and context menus.

## Later slices

- **b:** `SpaceModel` becomes the data source, using the
  `TabStripModel` ↔ `SpaceModel` bridge.
- **c:** pinned section, nested folders, drag and drop.
- **d:** persistence.
- **e:** remove the hidden native strip and take over drag/drop and hover
  cards.

## Risks

- **Initial WebUI is experimental upstream** (`webui_toolbar` is an
  experiment). If it changes, the sidebar can fall back to a plain preloaded
  `WebUIContentsWrapper`, as tab search uses. That's slower to first paint but
  stable.
- **Hiding instead of removing `TabStripView`** keeps upstream invariants
  intact, but costs some memory and observer work. Slice e removes it.
