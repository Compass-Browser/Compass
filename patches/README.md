# Patches

Small changes to upstream Chromium files that can't be done in `//strata` or
with a `chromium_src` override. There's one patch per file, named after its
path with `/` replaced by `-`.

| Location | Applies to |
|---|---|
| `patches/*.patch` | `src/` |
| `patches/<repo>/*.patch` | the nested git repository `src/<repo>/` (for example `patches/v8/`) |

## Workflow

```sh
# 1. Edit the upstream file directly in the checkout.
$EDITOR ../strata-chromium/src/chrome/browser/BUILD.gn

# 2. Regenerate patches/ from everything that differs from the pinned tag.
tools/strata patches export            # add --repo v8 for a nested repo

# 3. Commit the patch together with the //strata code that needs it.
```

Other commands: `strata patches status | apply | revert`. Applying is
idempotent, so patches that are already applied are skipped.

## Keep patches tiny

Every line here can conflict on each monthly Chromium update. A good patch
adds a hook (a dependency, a call into `//strata`, a friend declaration) and
nothing else.

## Current patches

| Patch | Why |
|---|---|
| `BUILD.gn.patch` | Adds `//strata:strata_unittests` to `gn_all` |
| `chrome-app-theme-chromium-BRANDING.patch` | Product name, company and bundle ID |
| `chrome-browser-BUILD.gn.patch` | Links `//strata/browser` into the browser, and `//strata/browser:feature_overrides` into `:core` |
| `chrome-browser-chrome_browser_field_trials.cc.patch` | Registers Strata's feature defaults (`strata::RegisterFeatureOverrides()`): browser-initiated auto picture-in-picture on |
| `components-content_settings-core-browser-content_settings_registry.cc.patch` | Auto picture-in-picture allowed by default: no prompt, no media engagement needed (docs/design/picture-in-picture.md) |
| `chrome-browser-picture_in_picture-auto_picture_in_picture_tab_helper.cc.patch` | Auto picture-in-picture replaces another tab's auto picture-in-picture, so the tab left last shows |
| `chrome-browser-picture_in_picture-auto_picture_in_picture_tab_strip_observer_helper.cc.patch` | A tab moved into the parking window (`strata::IsInParkingWindow()`) counts as switched away from |
| `chrome-browser-picture_in_picture-BUILD.gn.patch` | Lets that helper depend on `//strata/browser/window_sync` |
| `chrome-browser-ui-views-overlay-video_overlay_window_views.cc.patch` | On macOS the picture-in-picture window shows over other apps' fullscreen spaces and takes clicks without activating the browser |
| `chrome-browser-ui-BUILD.gn.patch` | Links `//strata/browser/ui` into `//chrome/browser/ui` and lets it include ui headers (same as upstream `tab_search`); `browser_command_controller_impl` depends on the command bar |
| `chrome-browser-ui-browser_command_controller.cc.patch` | `ExecuteCommandWithDispositionAndContext()` gives `strata::CommandBar` first refusal on New Tab and Focus Location (Cmd+T, Cmd+L), before upstream picks between its command handler and browser actions |
| `chrome-browser-ui-browser_window-internal-BUILD.gn.patch` | Links `//strata/browser/window_sync` into the window features |
| `chrome-browser-ui-browser_window-internal-embedder_browser_window_features.cc.patch` | Creates `strata::WindowFeatures` in the embedder hook's `Init()` |
| `chrome-browser-ui-browser_window-public-embedder_browser_window_features.h.patch` | The `strata::WindowFeatures` member |
| `chrome-browser-ui-views-frame-browser_view.cc.patch` | Calls `strata::HandOffTabsBeforeClose()` when a window is asked to close; `IsToolbarVisible()` is false while `strata::SidebarHeader` holds the toolbar's controls (`strata::ShouldHideToolbar()`); hands the toolbar to the header after `Init()` (`strata::AttachToolbar()`) and takes it back first thing in `~BrowserView()` (`strata::DetachToolbar()`) |
| `chrome-browser-ui-views-frame-vertical_tab_strip_region_view.cc.patch` | Constructs `strata::SidebarHeader` as its top container; `InitializeTabStrip()` adds the Strata sidebar in place of the native tab strip, which is never built; `GetTabData()` reads tab data from the tab; `strata::ActiveTabWatcher` reports active-tab changes; resize, collapse and hover-card lock paths no longer assume a native strip |
| `chrome-browser-ui-views-frame-vertical_tab_strip_region_view.h.patch` | Declares those overrides and the sidebar members |
| `chrome-browser-ui-tabs-tab_strip_prefs.cc.patch` | Vertical tabs on by default; tab search and the Everything menu unpinned from the tab strip by default (the command bar covers them) |
| `chrome-browser-ui-views-chrome_views_delegate.cc.patch` | `OnBeforeWidgetInit()` calls `strata::AdjustBubbleForSidebar()`: every bubble still anchored in the hidden toolbar moves to the sidebar header, and bubbles anchored in the header open toward the page |
| `chrome-browser-ui-views-location_bar-location_bar_view.cc.patch` | `RefreshContentSettingViews()` also updates the header's content setting icons (`strata::UpdateContentSettingsIcons()`), as it does the web app frame's |
| `chrome-browser-ui-views-toolbar-toolbar_view.cc.patch` | Bubble anchors in the hidden toolbar go to the header (`strata::GetToolbarBubbleAnchor()`); `FocusToolbar()` and `SetPaneFocusAndFocusAppMenu()` focus the header instead (`strata::FocusToolbarControls()`, `strata::FocusAppMenu()`) |
| `chrome-browser-ui-views-tabs-vertical-vertical_tab_strip_top_container.h.patch` | Makes `IsPositionInWindowCaption()` virtual, so `strata::SidebarHeader`'s buttons don't drag the window |
| `chrome-browser-ui-ui_features.cc.patch` | Hover card preview images on by default on macOS too (upstream turns them off there); the sidebar's hover cards show them |
| `chrome-browser-ui-webui-chrome_web_ui_configs.cc.patch` | Registers Strata's WebUI configs |
| `chrome-browser-chrome_browser_interface_binders_webui_parts_desktop.cc.patch` | Binds Strata's WebUI Mojo interfaces |
| `chrome-chrome_paks.gni.patch` | Ships `strata_sidebar_resources.pak` |
| `third_party-lit-v3_0-BUILD.gn.patch` | Allows `//strata/*` to depend on Lit |
| `tools-gritsettings-resource_ids.spec.patch` | Reserves resource IDs for the sidebar's grit file |
| `tools-metrics-histograms-metadata-page-histograms.xml.patch` | Adds the `.StrataCommandBar` WebUIName variant, required by `WebUIContentsWrapperT`'s static_assert |
| `chrome-browser-profiles-keep_alive-profile_keep_alive_types.h.patch` | Adds the `kStrataSpaces` keep-alive origin: `strata::SpaceDirectory` keeps profiles whose spaces show in the switcher loaded (D17) |
| `chrome-browser-profiles-keep_alive-profile_keep_alive_types.cc.patch` | Prints `kStrataSpaces` |
| `tools-metrics-histograms-metadata-profile-enums.xml.patch` | Adds `kStrataSpaces` to the `ProfileKeepAliveOrigin` histogram enum |
