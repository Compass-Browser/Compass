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
