# Patches

Small changes to upstream Chromium files that can't be done in `//compass` or
with a `chromium_src` override. There's one patch per file, named after its
path with `/` replaced by `-`.

| Location | Applies to |
|---|---|
| `patches/*.patch` | `src/` |
| `patches/<repo>/*.patch` | the nested git repository `src/<repo>/` (for example `patches/v8/`) |

## Workflow

```sh
# 1. Edit the upstream file directly in the checkout.
$EDITOR ../compass-chromium/src/chrome/browser/BUILD.gn

# 2. Regenerate patches/ from everything that differs from the pinned tag.
tools/compass patches export            # add --repo v8 for a nested repo

# 3. Commit the patch together with the //compass code that needs it.
```

`compass build` (and `compass test`) first bring the checkout in line with
patches/, as after a pull: they apply patches that aren't applied, update
ones whose file still has an older version of the patch (the file goes back
to the pinned tag, then the patch applies), and put back files whose patch
was removed. Only changes that match a committed version of a patch are
undone; anything else stops the build with the files to look at.
`--skip-patches` builds the checkout as it is.

Other commands: `compass patches status | apply | revert`. Applying is
idempotent, so patches that are already applied are skipped.

## Keep patches tiny

Every line here can conflict on each monthly Chromium update. A good patch
adds a hook (a dependency, a call into `//compass`, a friend declaration) and
nothing else.

## Current patches

| Patch | Why |
|---|---|
| `BUILD.gn.patch` | Adds `//compass:compass_unittests` to `gn_all` |
| `crypto-subtle_passkey.h.patch` | Lets `compass::arc::MakeCryptoPassKey()` call the PBKDF2 key derivation, to decrypt Arc's data with Chromium's macOS parameters (docs/design/arc-import.md) |
| `chrome-app-theme-chromium-BRANDING.patch` | Product name, company and bundle ID |
| `tools-grit-grit_rule.gni.patch` | The `grit` template builds the Chromium-branded strings in `compass_rebranded_grds` from renamed copies (`//compass/strings`), with their start IDs from `gen/compass/strings/resource_ids` (docs/design/branding.md) |
| `.gn.patch` | Lets `//compass/strings/BUILD.gn` call `exec_script` (it lists the copies it writes) |
| `chrome-browser-ui-tab_helpers.cc.patch` | Every tab gets `compass::AttachWebStoreBranding()`: the Chrome Web Store's buttons say Compass (docs/design/branding.md "Web Store"); `chrome-browser-ui-BUILD.gn.patch` adds the dependency |
| `*.png.patch`, `*.svg.patch`, `*.ico.patch`, `*.icon.patch` | Compass's logo in place of Chromium's (binary patches written by `tools/branding/make_patches.py`, docs/design/branding.md "Logo") |
| `components-os_crypt-common-keychain_password_mac.mm.patch`, `components-os_crypt-common-BUILD.gn.patch` | On macOS the safe storage key lives in the Keychain item "Compass Safe Storage" (account "Compass") instead of upstream's "Chromium Safe Storage"; when the new item is missing, `compass::MoveLegacyKeychainPassword()` (`components/os_crypt/`) copies the old item's key so saved passwords and cookies stay readable |
| `chrome-BUILD.gn.patch` | The macOS app icon comes from `assets/mac/` (`tools/build_mac_icon.sh` builds it from `assets/Compass.icon` and `assets/Compass.icns`) |
| `chrome-browser-BUILD.gn.patch` | Links `//compass/browser` into the browser, and `//compass/browser:feature_overrides`, `:url_handler`, `//compass/browser/native_messaging` and `//compass/browser/ui/cocoa:shortcuts` into `:core` |
| `chrome-browser-chrome_browser_main.cc.patch` | macOS: at launch, after creating the user's NativeMessagingHosts folder, `compass::CopyNativeMessagingHostsFromChrome()` copies 1Password's manifest there from Chrome's folder, so the 1Password extension can reach the desktop app (docs/design/native-messaging.md) |
| `chrome-browser-global_keyboard_shortcuts_mac.mm.patch` | The shortcuts outside the main menu go through `compass::AdjustShortcutsNotInMenu()`: Cmd+Shift+C copies the page's link (docs/design/shortcuts.md). `CommandForKeyEvent()` finds nothing while the settings page records keys (`compass::IsRecordingShortcut()`) and, after the menu, asks `compass::CustomCommandForKeyEvent()` for the user's shortcuts; `GetDefaultMacAcceleratorForCommandId()` asks `compass::CustomAcceleratorForCommand()` first (shortcuts.md "Customizing"); `DelayedWebContentsCommandForKeyEvent()` asks `compass::DelayedCommandForKeyEvent()` first, for Cmd+Z and Cmd+Shift+Z once the page passed on them (sidebar.md "Undo closing") |
| `chrome-browser-ui-cocoa-accelerators_cocoa.mm.patch` | The main menu's key equivalents go through `compass::AdjustMenuAccelerators()`: Cmd+S opens or closes the sidebar, Save Page As has none; it keeps the map to apply the user's shortcuts to |
| `chrome-browser-ui-startup-infobar_utils.cc.patch` | No "Google API keys are missing" bar at launch: Compass is built without them on purpose (docs/design/privacy.md) |
| `chrome-install_static-chromium_install_modes.h.patch` | Windows: Compass's own product path, app name and ID, ProgIDs and COM CLSIDs (docs/design/branding.md "Windows") |
| `chrome-installer-util-BUILD.gn.patch` | Windows: installer strings (shortcut names) from the renamed `chromium_strings.grd` copy (docs/design/branding.md "Windows") |
| `chrome-common-chrome_paths_mac.mm.patch` | macOS: the user data folder is "Compass", moved from "Chromium" on first launch (`compass::GetMacProductDirName()`, docs/design/branding.md "User data folder"); `chrome-common-BUILD.gn.patch` adds the dependency |
| `chrome-browser-prefs-browser_prefs.cc.patch` | Registers Compass's Local State prefs, the user's shortcuts (`compass::RegisterShortcutPrefs()`), and its profile prefs (`compass::RegisterCompassProfilePrefs()`: what was imported from Arc). Last in `RegisterLocalState()` and `RegisterProfilePrefs()`, `compass::OverrideLocalStatePrefDefaults()` and `compass::OverrideProfilePrefDefaults()` set Compass's defaults for upstream prefs: no profile picker at startup, no password leak check, no error page probes (docs/design/privacy.md). `chrome-browser-prefs-BUILD.gn.patch` adds the dependencies |
| `chrome-browser-ui-cocoa-BUILD.gn.patch` | Lets `accelerators_cocoa.mm` depend on `//compass/browser/ui/cocoa:shortcuts` |
| `chrome-browser-chrome_browser_field_trials.cc.patch` | Registers Compass's feature defaults (`compass::RegisterFeatureOverrides()`): browser-initiated auto picture-in-picture on |
| `components-content_settings-core-browser-content_settings_registry.cc.patch` | Auto picture-in-picture allowed by default: no prompt, no media engagement needed (docs/design/picture-in-picture.md) |
| `chrome-browser-picture_in_picture-auto_picture_in_picture_tab_helper.cc.patch` | Auto picture-in-picture replaces another tab's auto picture-in-picture, so the tab left last shows |
| `chrome-browser-picture_in_picture-auto_picture_in_picture_tab_strip_observer_helper.cc.patch` | A tab moved into the parking window (`compass::IsInParkingWindow()`) counts as switched away from |
| `chrome-browser-picture_in_picture-BUILD.gn.patch` | Lets that helper depend on `//compass/browser/window_sync` |
| `chrome-browser-ui-views-overlay-BUILD.gn.patch` | The overlay target depends on `//compass/browser/ui/views/picture_in_picture:pip_windows` on macOS |
| `chrome-browser-ui-views-overlay-video_overlay_window_views.cc.patch` | On macOS the picture-in-picture window shows over other apps' fullscreen spaces and takes clicks without activating the browser; and hands the window to `compass::PipWindows` (click-through, Ctrl+Option+Cmd+P) |
| `chrome-browser-ui-BUILD.gn.patch` | Links `//compass/browser/ui` into `//chrome/browser/ui` and lets it include ui headers (same as upstream `tab_search`); `browser_command_controller_impl` depends on the command bar; `browser_commands_impl` on `//compass/common:url`; `accelerator_table` on `//compass/browser/ui:app_menu_hooks` (the command IDs) |
| `chrome-browser-ui-accelerator_table.cc.patch` | Windows: Shift+Alt+N opens a little window (`compass::kNewLittleWindowCommand`) instead of New Split Tab (docs/design/little-window.md) |
| `chrome-browser-ui-browser_command_controller.cc.patch` | `ExecuteCommandWithDispositionAndContext()` gives `compass::CommandBar` first refusal on New Tab and Focus Location (Cmd+T, Cmd+L), before upstream picks between its command handler and browser actions; Compass's own commands (`compass_command_ids.h`, Cmd+Option+N) go to `compass::HandleBrowserCommand()` before the supported/enabled check; `IsCommandEnabled()` enables Reopen Closed Tab while `compass::IsBrowserCommandEnabled()` says a closed sidebar entry can come back (docs/design/sidebar.md "Undo closing") |
| `chrome-browser-ui-browser_window-internal-BUILD.gn.patch` | Links `//compass/browser/window_sync` into the window features |
| `chrome-browser-ui-browser_window-internal-embedder_browser_window_features.cc.patch` | Creates `compass::WindowFeatures` in the embedder hook's `Init()` |
| `chrome-browser-ui-browser_window-public-embedder_browser_window_features.h.patch` | The `compass::WindowFeatures` member |
| `chrome-browser-ui-views-frame-browser_view.cc.patch` | Calls `compass::HandOffTabsBeforeClose()` when a window is asked to close; `IsToolbarVisible()` is false while `compass::SidebarHeader` holds the toolbar's controls (`compass::ShouldHideToolbar()`); hands the toolbar to the header after `Init()` (`compass::AttachToolbar()`) and takes it back first thing in `~BrowserView()` (`compass::DetachToolbar()`); `NonClientHitTest()` lets the window frame around the page drag the window (`compass::IsOnWindowFrame()`), and a little window's frame and bar (`compass::IsOnLittleWindowCaption()`) |
| `chrome-browser-ui-views-frame-vertical_tab_strip_region_view.cc.patch` | Constructs `compass::SidebarHeader` as its top container; `InitializeTabStrip()` adds the Compass sidebar in place of the native tab strip, which is never built; `GetTabData()` reads tab data from the tab; `compass::ActiveTabWatcher` reports active-tab changes; resize, collapse and hover-card lock paths no longer assume a native strip; at the collapsed width its children are hidden (docs/design/window-frame.md) |
| `chrome-browser-ui-views-frame-vertical_tab_strip_region_view.h.patch` | Declares those overrides and the sidebar members; `kCollapsedWidth` is `compass::kCollapsedSidebarWidth`, the window frame's edge |
| `chrome-browser-ui-views-frame-layout-browser_view_tabbed_layout_impl.cc.patch` | The window frame (docs/design/window-frame.md): insets the page beside the vertical region by `compass::GetContentFrameInsets()` (below the caption buttons on Windows) and rounds its corners; no top separator, floating corner pieces, concave region corner or region outline (except shown on hover); the collapsed region starts at the window's top |
| `chrome-browser-ui-views-frame-BUILD.gn.patch` | Lets that layout depend on `//compass/browser/ui/views/frame:content_frame` |
| `chrome-browser-ui-tabs-tab_strip_prefs.cc.patch` | Vertical tabs on by default; tab search and the Everything menu unpinned from the tab strip by default (the command bar covers them) |
| `chrome-browser-ui-views-chrome_views_delegate.cc.patch` | `OnBeforeWidgetInit()` calls `compass::AdjustBubbleForSidebar()`: every bubble still anchored in the hidden toolbar moves to the sidebar header, and bubbles anchored in the header open toward the page |
| `chrome-browser-ui-views-location_bar-location_bar_view.cc.patch` | `RefreshContentSettingViews()` also updates the header's content setting icons (`compass::UpdateContentSettingsIcons()`), as it does the web app frame's; a little window's location bar is a pill, not the popup's rectangle |
| `chrome-browser-ui-views-toolbar-toolbar_view.cc.patch` | Bubble anchors in the hidden toolbar go to the header (`compass::GetToolbarBubbleAnchor()`); `FocusToolbar()` and `SetPaneFocusAndFocusAppMenu()` focus the header instead (`compass::FocusToolbarControls()`, `compass::FocusAppMenu()`) |
| `chrome-browser-ui-views-tabs-vertical-vertical_tab_strip_top_container.h.patch` | Makes `IsPositionInWindowCaption()` virtual, so `compass::SidebarHeader`'s buttons don't drag the window |
| `chrome-browser-ui-ui_features.cc.patch` | Hover card preview images on by default on macOS too (upstream turns them off there); the sidebar's hover cards show them |
| `chrome-browser-ui-webui-chrome_web_ui_configs.cc.patch` | Registers Compass's WebUI configs |
| `chrome-browser-chrome_browser_interface_binders_webui_parts_desktop.cc.patch` | Binds Compass's WebUI Mojo interfaces, and on chrome://settings those of Compass's sections (docs/design/settings.md) |
| `chrome-browser-ui-webui-settings-settings_ui.{h,cc}.patch`, `chrome-browser-ui-webui-settings-BUILD.gn.patch` | chrome://settings owns a `compass::SettingsSections`: it serves Compass's resources under compass/ and binds the sections' interfaces (a templated `BindInterface()`) (docs/design/settings.md) |
| `chrome-browser-resources-settings-BUILD.gn.patch`, `...-settings_main-settings_main.{ts,html}.patch`, `...-route.ts.patch`, `...-router.ts.patch`, `...-settings_menu-settings_menu.html.patch` | Compass's settings sections (Sync, Keyboard shortcuts, Import from Arc): `/compass/` mapped to `//compass/resources/sidebar` (as `/shared/settings/`), their imports, routes (`/sync`, `/shortcuts`, `/importArc`), menu items and views |
| `chrome-chrome_paks.gni.patch` | Ships `compass_sidebar_resources.pak` |
| `third_party-lit-v3_0-BUILD.gn.patch` | Allows `//compass/*` to depend on Lit |
| `tools-gritsettings-resource_ids.spec.patch` | Reserves resource IDs for the sidebar's grit file |
| `tools-metrics-histograms-metadata-page-histograms.xml.patch` | Adds the `.CompassCommandBar` WebUIName variant, required by `WebUIContentsWrapperT`'s static_assert |
| `chrome-browser-profiles-keep_alive-profile_keep_alive_types.h.patch` | Adds the `kCompassSpaces` keep-alive origin: `compass::SpaceDirectory` keeps profiles whose spaces show in the switcher loaded (D17) |
| `chrome-browser-profiles-keep_alive-profile_keep_alive_types.cc.patch` | Prints `kCompassSpaces` |
| `tools-metrics-histograms-metadata-profile-enums.xml.patch` | Adds `kCompassSpaces` to the `ProfileKeepAliveOrigin` histogram enum |
| `ui-views-widget-widget.h.patch`, `ui-views-widget-widget.cc.patch` | `Widget::ExchangeContentsWith()`: two widgets swap non-client views and delegates, with new frame views (profiles in place, D18) |
| `chrome-browser-ui-views-frame-browser_view.h.patch`, `-browser_widget.h.patch`, `-browser_root_view.h.patch` | `friend class compass::BrowserViewExchange`, which re-points their `BrowserView` members |
| `chrome-browser-ui-views-frame-browser_native_widget_mac.h.patch`, `.mm.patch` | The same friend, and `RebindTouchBar()`: the touch bar for the view now in the window; a little window gets a normal window's NSWindow with no title and a title bar as tall as its bar (`compass::IsCreatingLittleWindow()`, `compass::GetLittleWindowBarHeight()`) |
| `chrome-browser-ui-views-frame-tab_modal_dialog_host.cc.patch` | Re-observes the widget when the contents move to another one, instead of observing twice |
| `chrome-browser-ui-views-extensions-extension_keybinding_registry_views.h.patch`, `.cc.patch` | `SetFocusManager()`: extension shortcuts follow the view to its new widget |
| `chrome-browser-ui-views-color_provider_browser_helper.h.patch`, `.cc.patch` | `SetColorProviderSource()`: pages take colors from the widget the view is in |
| `chrome-browser-ui-browser_web_contents_delegate-browser_web_contents_delegate.cc.patch` | `ActivateContents()` asks `compass::RevealParkedTab()` first: a parked tab (picture-in-picture's "back to tab", `window.focus()`) shows in the window the user was in, in its space, never in the parking window; `OpenURLFromTab()` gives Option+clicked links (`SAVE_TO_DISK`) to `compass::PeekOrDownload()`, and `AddNewContents()` new foreground tabs from pinned tabs to `compass::MaybePeekNewContents()` (docs/design/peek.md) |
| `chrome-common-chrome_content_client.cc.patch`, `chrome-common-BUILD.gn.patch` | Registers `compass` as a standard scheme in every process (branding.md "compass://") |
| `chrome-browser-chrome_content_browser_client.cc.patch` | `BrowserURLHandlerCreated()` adds `compass::AddCompassURLHandler()` first: compass://x loads chrome://x |
| `chrome-browser-profiles-profile_io_data.cc.patch`, `chrome-browser-profiles-BUILD.gn.patch` | `compass` is a handled protocol: the omnibox takes compass:// as a URL, startup and context menus accept it, and it never goes to the OS |
| `chrome-browser-ui-toolbar-chrome_location_bar_model_delegate.cc.patch`, `chrome-browser-ui-toolbar-BUILD.gn.patch` (1st hunk) | The location bar shows chrome:// URLs as compass:// |
| `chrome-browser-ui-toolbar-app_menu_model.cc.patch`, `chrome-browser-ui-toolbar-BUILD.gn.patch` (2nd hunk) | The app menu has a sync row above Settings (`compass::GetAppMenuSyncLabel()`, command `compass::kOpenSyncPageCommand`, run by the command controller's Compass hook); Compass commands are always enabled in the menu (docs/design/sync.md, "Slice 4 notes") |
| `chrome-browser-ui-browser_commands.cc.patch` | Copy URL (Cmd+Shift+C) copies chrome:// URLs as compass:// |
| `content-renderer-render_frame_impl.cc.patch` | An Option+clicked GET http(s) link goes to the browser as `OpenURL` with `SAVE_TO_DISK` instead of `DownloadURL`, so the browser can peek at it (docs/design/peek.md) |
| `third_party-blink-renderer-core-html-html_anchor_element.cc.patch` | Option+click on a link navigates at once, without upstream's double-click delay, so Peek opens on the click |
| `chrome-browser-app_controller_mac.mm.patch` | URLs from other apps open in little windows: `OpenStartupTabsInBrowserWithProfile()` asks `compass::OpenInLittleWindows()` first (docs/design/little-window.md) |
| `chrome-browser-ui-startup-startup_browser_creator_impl.cc.patch`, `chrome-browser-ui-startup-BUILD.gn.patch` | Windows: links from other apps (the command line's URLs) open in little windows: with Compass running, only those; at startup, after the usual windows, which open without them (`compass::CanOpenInLittleWindows()`, `OpenInLittleWindows()`, docs/design/little-window.md) |
| `chrome-browser-ui-views-frame-browser_caption_button_container_win.cc.patch` | Windows: the caption buttons always paint to a transparent layer, so they draw over Compass's frame, which fills the browser view (docs/design/window-frame.md "Windows") |
| `chrome-browser-ui-views-frame-layout-browser_view_popup_layout_impl.cc.patch` | A little window's page sits in a card inside the frame (`compass::GetPopupContentFrameInsets()`), with no separator |
| `components-sync-service-BUILD.gn.patch` | `//components/sync/service` depends on `//compass/components/sync:engine` (docs/design/sync.md, "Slice 2 notes") |
| `components-sync-service-sync_service_impl.cc.patch` | In `TryStartImpl()`, local sync with a Compass chain uses `compass::sync::CreateEngineComponentsFactory()`: the engine talks to the chain's server instead of the loopback file (route 2) |
| `chrome-browser-sync-sync_service_factory.cc.patch` | `WEBAUTHN_CREDENTIAL` is a disabled sync type (passkeys need Google's security domain; no flag turns it off) |
| `chrome-browser-profiles-chrome_browser_main_extra_parts_profiles.cc.patch`, `chrome-browser-profiles-BUILD.gn.patch` (2nd hunk) | Builds `compass::sync::CompassSyncServiceFactory` with the other factories, which registers the `compass.sync.*` prefs and creates the service with each profile |
| `components-history-core-browser-sync-delete_directive_handler.h.patch`, `components-history-core-browser-sync-delete_directive_handler.cc.patch`, `components-history-core-browser-sync-history_delete_directives_data_type_controller.cc.patch` (2nd hunk) | Under local sync, history delete directives processed at startup stay on the server until their end time is 30 days old, instead of being deleted at once, so a device that joins later still deletes that history (docs/design/sync.md, "Slice 7 notes") |
| `chrome-browser-ui-startup-first_run_service.cc.patch` | No first-run experience: `ShouldOpenFirstRun()` is false, so no `FirstRunService` is built and the first window opens right away. `IsChromeFirstRun()` stays true on the first run (docs/design/privacy.md) |
| `chrome-browser-ui-views-profiles-profile_menu_view.cc.patch` | The profile menu has no "Google services settings" button: there's no browser sign-in |
| `components-unified_consent-unified_consent_service.cc.patch` | URL-keyed anonymized data collection is never turned on automatically, for instance when Compass sync turns History on; the user can still turn it on in Settings |
| `chrome-browser-signin-account_consistency_mode_manager.cc.patch` | No browser sign-in to a Google account: `CanEnableDiceForBuild()` is false even with an OAuth client configured, so `signin.allowed` is false at every start and there's no account consistency (docs/design/privacy.md) |
