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
| `chrome-browser-BUILD.gn.patch` | Links `//strata/browser` into the browser |
| `chrome-browser-ui-BUILD.gn.patch` | Links `//strata/browser/ui` into `//chrome/browser/ui` and lets it include ui headers (same as upstream `tab_search`) |
| `chrome-browser-ui-browser_window-internal-BUILD.gn.patch` | Links `//strata/browser/window_sync` into the window features |
| `chrome-browser-ui-browser_window-internal-embedder_browser_window_features.cc.patch` | Creates `strata::WindowFeatures` in the embedder hook's `Init()` |
| `chrome-browser-ui-browser_window-public-embedder_browser_window_features.h.patch` | The `strata::WindowFeatures` member |
| `chrome-browser-ui-views-frame-vertical_tab_strip_region_view.cc.patch` | Calls `strata::InstallSidebar()` when the region adds its tab strip, and passes the bottom container |
| `chrome-browser-ui-tabs-tab_strip_prefs.cc.patch` | Vertical tabs on by default |
| `chrome-browser-ui-webui-chrome_web_ui_configs.cc.patch` | Registers Strata's WebUI configs |
| `chrome-browser-chrome_browser_interface_binders_webui_parts_desktop.cc.patch` | Binds Strata's WebUI Mojo interfaces |
| `chrome-chrome_paks.gni.patch` | Ships `strata_sidebar_resources.pak` |
| `third_party-lit-v3_0-BUILD.gn.patch` | Allows `//strata/*` to depend on Lit |
| `tools-gritsettings-resource_ids.spec.patch` | Reserves resource IDs for the sidebar's grit file |
