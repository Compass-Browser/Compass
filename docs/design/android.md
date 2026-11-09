# Compass for Android

A Chromium-based browser for Android phones and tablets. It joins the same
sync chain as the desktop and shows the same spaces and sidebar in a native
Kotlin + Jetpack Compose interface. Feature for feature, it matches the iOS
app (ios.md).

**Working on it:**

- `tools/compass gen portable --cpu android-arm64` sets up
  `out/Portable-android-arm64`.
- `tools/compass build -c portable --cpu android-arm64 compass_apk` builds
  `apks/Compass.apk` (`it.cipi1965.compass`): Chrome's browser with
  Compass's shell (`//compass/android`). `chrome_public_apk` still builds
  plain Chromium from the same tree, without the shell.
- Building from a Mac takes a few workarounds, which the tool applies:
  building.md, "Android from a Mac".
- Testing happens on an arm64 Android emulator on an Apple-silicon Mac
  (or Genymotion SaaS arm64 devices), with adb.

## Context

On iOS, WebKit is the only engine allowed, so the iOS app is a separate
Swift client: SwiftUI, WKWebView, and a Swift port of the sync stack. Android
has no such limit, so the Android app is built from the same Chromium
checkout, patches and `//compass` tree as the desktop.

This gets us:

- **One engine.** The same Blink on every device, whatever WebView the
  device has, or doesn't have.
- **The desktop's code.** The C++ sync engine runs on Android as is: chain,
  pairing, Nigori, `COMPASS_SPACES`, passwords, history, sessions and
  send-tab. So do the sidebar model and the Claude agent loop.
- **Extensions.** These come through upstream's desktop-Android work (below).

It costs:

- **Build time.** Builds are Chromium-sized: a full build is hours, while
  incremental builds are minutes.
- **Rebasing.** Every Chromium update rebases the Android side of the
  patches too, so the Android UI lives in `//compass` as far as possible and
  patches upstream files only for hooks.

**Decisions:**

- **Foundation:** a Chromium fork with Compass's own shell in Kotlin and
  Jetpack Compose. Chromium 156 builds Kotlin and Compose itself
  (`enable_compose` in its Android templates, Compose and Material 3 in
  `third_party/androidx`), and Chrome already ships Compose UI
  (`chrome/browser/ui/android/bricks/`).
- **UI scope:** the main screens are Compass's own. That covers the bottom
  bar, sidebar, spaces, tab picker, command bar, theme editor, pairing and
  Claude. Chrome's secondary screens (settings sub-pages, permission prompts,
  page info) stay, re-themed.
- **Extensions:** included from the start, through `is_desktop_android`.
- **Distribution:** Play internal testing, plus a signed APK on the GitHub
  release. No F-Droid: its build servers can't practically build a Chromium
  fork.
- **Push:** FCM, through a Compass Firebase project.
- **Target:** arm64 phones first, with an adaptive layout for tablets and
  foldables.

## Architecture

```
//compass
  android/                    the app: GN targets (android_apk templates, enable_compose = true)
    AndroidManifest.xml       it.cipi1965.compass
    java/src/it/cipi1965/compass/   Kotlin: CompassActivity, Compose UI, JNI bridge classes
    res/
  browser/android/            C++ halves of the bridges (jni_zero): spaces, pairing, sync status, Claude
  browser/sync, components/sync   shared with the desktop
  browser/sidebar             SpaceModel; its model part builds for Android, its Views/WebUI parts stay desktop-only
  browser/claude              shared agent loop and tools (common/claude/tools.json)
  config/args/android.gn      Android GN args, imported for --cpu android-arm64
  patches/                    shared; Android hunks are guarded by is_android, desktop-only ones by !is_android
```

- **The shell today (C1 spike).** `ChromeTabbedActivity` hands itself to
  `CompassShellHook` at the end of native initialization (a two-line patch;
  `ServiceLoaderUtil` finds the implementation, which only `compass_apk`
  has). `CompassBar` adds a Compose bar as its own layer of Chrome's bottom
  controls stack (`COMPASS_BAR`), so pages make room for it, and follows
  the current tab: back, the address, the tab count (Chrome's tab
  switcher), new tab and Chrome's menu. Chrome's top toolbar is hidden
  (`CompassShell.isPresent()`, checked by a patched
  `TopToolbarCoordinator`).
- **Spaces (C2, C3).** `//compass/browser/android` runs the desktop's
  model on Android: `SpacesAndroid` (per profile) owns the `SpaceList` and
  its `SidebarStore` (the same "Compass Sidebar.json") and maps Chrome's
  tab IDs to nodes: a new tab is a Today entry, a restored one takes its
  saved entry, a closed one leaves Today (a pinned tab or favorite stays,
  unloaded). `SpacesBridge` (jni_zero) gives Java a JSON snapshot of what
  the sidebar shows and takes the shell's tab events and actions. The
  sidebar (`SidebarUi.kt`) is the iOS app's panel up from the bottom (a
  swipe up on the bar's address; full or half height, its grabber moves
  it), in the space's tint: the favorites grid, the space's name and ⋯ menu
  (rename, theme, delete), Pinned, Today with Clear and New Tab, and a bar
  with Settings, the spaces and a new tab; a sideways swipe goes to the
  next space. A long press on an entry pins it, makes it a favorite, moves
  it to Today or to another space (with its tab), copies its link, or
  closes or unpins it; unpinning an entry whose tab is open leaves the tab
  in Today.
  Chrome's tabs of every space stay in the window's TabModel; the sidebar
  shows the window's space's. Chrome's own archiving of inactive tabs is
  off (`TabArchiveSettings` patch): Today and the archive do that.
- **Sync (C2, C5).** `CompassSyncService` runs on Android as on the
  desktop (chain, pairing, Nigori, the notifier), without what needs
  windows: the refresh on a window's activation becomes the app coming
  back (`ApplicationStatusListener`), there's no space directory
  (`space_directory_android.cc`: one profile) and no creation of the
  chain's other profiles. COMPASS_SPACES runs the desktop's bridge on
  `SpacesAndroid` (`compass_spaces_sync_android.cc`): an entry is in use
  while it has a tab. The sync screen (`SyncScreen`, from the sidebar's
  Sync row) joins a chain with a pairing code or the recovery code,
  starts one, adds devices and approves them, and turns data types on
  and off, through `SyncBridge` (the desktop's `SyncController`). A
  desktop's pairing QR code (`compass-pair:?server=…&code=…`, sync.md)
  opens in Compass from the camera (`PairInviteActivity`, which registers
  the scheme) and joins with it.
- **Claude (C8).** The desktop's Claude client, key storage and page
  reader (`//compass/browser/claude`, without Claude Code) build for
  Android. `ClaudeBridge` runs a chat about the page on screen: each
  question carries the page's text (read in an isolated world, as on the
  desktop; incognito pages stay out), and the reply streams into a
  Compose sheet (`ClaudeChatUi.kt`, the bar's ✦). The key is the
  desktop's encrypted `ClaudeCredentials`. Claude acts with the desktop's
  tools (`ClaudeTools`): the page tools (read, find, click, type, scroll,
  navigate, screenshot) on the tab the question is about, and, with "Let
  Claude use the whole browser" on (a switch in the sheet, the desktop's
  pref), list_tabs, open_tab, select_tab and close_tab over Chrome's tab
  models (`ClaudeTabsAndroid`; a tab's id is its Android tab ID). The
  bridge runs the loop the desktop's chat page runs: a reply that asks for
  tools gets their results and goes on, up to 20 rounds a question. The
  sheet shows each tool as a quiet line between the replies. tools.json's
  `platforms` gains "android"; the desktop-only tools (spaces, folders,
  splits, history, downloads) aren't offered.
- **Wide windows (C10).** At 840dp and wider (tablets, unfolded
  foldables, desktop windows), the sidebar is a pane on the left, as on the
  desktop: `Spaces` moves Chrome's coordinator over by its width and puts
  the pane next to it, and follows the window's size. Narrower, it's the
  drawer. With a keyboard, the shell takes the desktop's Compass keys
  (Ctrl for Cmd) before Chrome's (`CompassBar.dispatchKeyEvent`, through
  the activity patch): Ctrl+S puts the pane away or opens the drawer,
  Ctrl+T, Ctrl+L and Ctrl+K open the command bar, Ctrl+Shift+E opens
  Claude and Ctrl+Shift+C copies the URL. Chrome's other shortcuts stay.
- **Multiple windows (C10).** Each window shows a space of its own, as on
  the desktop (`SpacesBridge`, one per window, keeps it; the start page
  follows its window's). The list's active space is the last one a window
  switched to, and new windows start on it. Tabs opened in a window go to
  its space's Today. Opening an entry whose tab is in another window moves
  the tab here with Chrome's own move between windows, so page state
  survives (sidebar.md, "Move on focus"); a moved tab keeps its entry.
- **Passwords in other apps (C6)** (`it.cipi1965.compass.autofill`), as
  the iOS AutoFill extension does: `CompassAutofillService` is an Android
  autofill service. On another app's login form (or a page in another
  browser) it offers "Compass passwords…", which opens the picker
  (`PasswordPickerActivity`): it asks for the screen lock (if there's one),
  starts Compass's browser process if it isn't running, and lists the
  saved passwords (`PasswordsBridge`, over the profile's password store),
  the app's or site's first: the site matching the form's web domain, or
  an app's package words (com.example.app: example). The one picked fills
  the form; only its password crosses to Java. Compass's own pages fill
  through Chrome's autofill. The sidebar's "Passwords in other apps" opens
  Android's setting to choose Compass. Saving from other apps isn't
  offered.
- **Extras (C9),** in `it.cipi1965.compass.extras` and the manifest's
  `extra_application_definitions`: a Quick Settings tile (new tab with the
  command bar), a search widget, a "Pin to Compass" share target, and a
  launcher shortcut per space (dynamic, from `Spaces`). They reach the
  shell through `CompassIntents` (its own actions to Chrome's launcher
  alias), which `CompassBar` handles at launch and on new intents.
- **The bar scrolls away** with the page, as on iOS: it's a layer of
  Chrome's bottom controls that scrolls off (`DEFAULT_SCROLL_OFF`), and
  while it moves the compositor draws a copy of it in step with the page
  (`ScrollingBottomViewSceneLayer` over the bar's `ViewResourceFrameLayout`,
  as Chrome's own bottom toolbar does); the view comes back once the bar is
  all the way in.
- **Tab picker (C4)** (`TabPicker`, `TabPickerUi.kt`), from a swipe up on
  the bar left of the address, as on iOS: the window's tabs as cards in a row over the space's
  tint, least recently shown at the left and the tab on screen centered.
  A tap shows a card's tab, a flick up closes it (Chrome's undo snackbar
  brings it back), a drag down closes the picker; it switches between
  regular and incognito tabs (the glasses, when there are some), and its
  one button opens a new tab. Thumbnails are Chrome's (`TabContentManager`),
  the tab on screen captured as the picker opens.
  Incognito tabs lock with Chrome's own setting ("Lock Incognito tabs when
  you leave", in Privacy and security): its unlock screen comes up
  whenever the window turns to them, from the picker too.
- **The command bar** (`CommandBar`, `CommandBarUi.kt`), as on iOS: a
  sheet in the space's tint with the field ("New tab in <space>", or "Go
  to" with the page's address selected) and cards: the typed address or
  search, the sidebar's entries to switch to (Today's while the field is
  empty), search suggestions and history, from Chrome's
  `AutocompleteController`.
- **The start page** (`StartPage`, `StartPageUi.kt`) replaces Chrome's new
  tab page for regular tabs (`NativePageFactory` patch; incognito keeps
  Chrome's): as the iOS app's empty page, the space's tint. As on the
  desktop's empty tab, a tab on it has no sidebar entry until it goes
  somewhere (`SpacesAndroid`).
- **Look.** The shell follows the iOS app's look (ios.md; Theme.swift's
  tokens in `CompassStyle.kt`), not Material You:
  - iOS's neutral system colors as Compose's color scheme (`CompassTheme`),
    the space's tint behind sheets (`spaceTint`), frosted floating bars
    (`floatingGlass`, an opaque fill and shadow: Compose can't blur what's
    behind a view), cards and rows (`Card`, `CardSection`, `CardRow`) and
    site icons from Chrome's favicon database (`Favicons`, `SiteIcon`).
  - Icons are Material Symbols Rounded, weight 500, in place of SF Symbols
    (`android/res_symbols`, `android/material_symbols.md`).
  - **The bar** (`CompassBarUi.kt`) is the iOS app's: back, forward, the
    address (the site's icon and host), + and ⋯ in a floating capsule.
    Swiping up on it opens, by where the finger starts, the tab picker
    (left of the address), the sidebar (on it) or the command bar (right
    of it); a sideways swipe goes to the next or previous tab. A long press
    on the address has Reload, Copy URL, Share, Find on Page and the
    desktop or mobile site; one on + a new incognito tab.
  - **The page menu** (`PageMenu`, `PageMenuUi.kt`), from ⋯, replaces
    Chrome's app menu: the address with back, forward, copy and reload;
    Find, Ask Claude, Pin and Share; Downloads and History; the window's
    extensions; Site Settings and Settings. Its actions are the bar's or
    Chrome's own (`onMenuOrKeyboardAction` with its menu item IDs).
  - **Find on page** (`FindBar`, `FindBarUi.kt`) is Compass's: a floating
    capsule over the keyboard with the text, "2 of 7", the previous and
    next match and Done, over Chrome's `FindInPageBridge`. Chrome's find
    bar is part of the toolbar the shell hides.
  - **Undo closing**: tabs closed from the sidebar, the picker or Today's
    Clear show iOS's toast over the bar ("Closed “Title”", Undo; four
    seconds, or a flick down) instead of Chrome's snackbar
    (`UndoToast`, `UndoToastUi.kt`; the `UndoBarController` patch hands the
    closure to it, and Undo and the timeout end it as the snackbar would).
    A tab brought back gets a new Today entry.
  - **Settings** (`CompassSettings`, `CompassSettingsUi.kt`), from the
    sidebar's gear and the page menu: Sync, then the Chrome settings pages
    Compass keeps, each opened at its page (search engine, passwords, clear
    browsing data, site settings, privacy, …), and Passwords in Other Apps,
    Claude and Extensions.
- **Branding so far.** The app is "Compass" with its own launcher icon (an
  adaptive icon from the glyph, `android/res/`, overlaying Chrome's
  `ic_launcher`), and there's no first-run experience.
- **The shell.** `CompassActivity` keeps Chrome's browser machinery: native
  start-up, Profile, the TabModel, the compositor, downloads, permissions,
  autofill, payments and Custom Tabs. Chrome's toolbar and tab switcher are
  replaced with a Compose UI in a `ComposeView`. Milestone C1 settles where
  exactly the shell plugs in. The first candidate is an activity derived
  from ChromeActivity with its own layout.
- **Bridges.** Kotlin sees the C++ models through jni_zero.
  - Spaces and the sidebar tree come up as `StateFlow`s, and user actions
    go down as calls. The Android UI therefore runs on the desktop's model
    rules: `SpaceModel::CanMove`, UniquePosition, client tags, Today and
    auto-archive.
  - Pairing, sync status and Claude get bridges of their own.
- **Versions.** Compose and AndroidX are the versions Chromium pins
  (`third_party/androidx/bill_of_materials.json`); they move with Chromium
  updates.

## Features

| iOS (ios.md) | Android |
|---|---|
| Floating bottom bar, collapses on scroll, swipe between tabs | A Compose bottom bar on Chromium's browser-controls offsets; a horizontal swipe on the bar switches tabs |
| Tab picker, command bar | A Compose grid; an M3 search bar over Chromium's omnibox suggestions, history, spaces and "Tabs on <device>" |
| Sidebar: spaces, favorites, pinned tree, Today, archive | A Compose drawer on phones and a permanent pane on tablets (window size classes), fed by the SpaceModel bridge |
| Space themes and editor | A Compose editor; the theme drives the M3 color scheme and Chrome's theme colors |
| Undo closing, incognito lock | Chromium's TabModel undo, shown as a snackbar; the incognito profile behind a BiometricPrompt lock |
| Passwords and the AutoFill extension | Chromium's password manager on the PASSWORDS sync type, plus its Android Autofill and Credential Manager integration for other apps |
| History, sessions, devices, sent tabs | Chromium's own sync types against `server/`; sent tabs arrive by FCM |
| Web extensions | Chromium's desktop-Android extensions, with installs from the Chrome Web Store |
| Content blocking | The desktop's |
| Downloads, PiP, find, desktop site, multiple windows | Chromium's own |
| Widgets, Control Center control, quick actions, share sheet | App widgets, a Quick Settings tile, dynamic shortcuts per space, and a share target that pins to a space |
| Claude | A Compose bottom sheet over the shared C++ agent; tools from `common/claude/tools.json`, whose `platforms` gains `"android"` |
| Pairing: QR code, code, recovery code | A Compose flow over the desktop's pairing code (side B) |

## Extensions

Upstream Chromium is bringing extensions to desktop-class Android devices.
The GN arg `is_desktop_android` turns on `enable_desktop_android_extensions`,
and code ported so far is guarded by `enable_extensions_core`
(`extensions/buildflags/buildflags.gni`). Compass builds with it on
(`config/args/android.gn`). `enable_desktop_android_extensions` alone
doesn't build: the extensions code also needs what `is_desktop_android`
brings, such as the WebUI new tab page.

The first targets are installs from the Chrome Web Store, 1Password and
uBlock. Gaps upstream hasn't filled yet get `//compass` code or small
guarded patches.

Upstream's extensions UI (the puzzle menu, pinned actions, popups) is part
of Chrome's tablet toolbar, which Compass replaces. Compass has its own
extensions sheet instead (`Extensions`, `ExtensionsUi.kt`), opened from the
menu's extensions item: it lists the window's extension actions through
upstream's `ExtensionsToolbarBridge`, runs one on a tap, and shows its popup
with Chrome's `ExtensionActionPopup` anchored to Compass's bar. It also
links to chrome://extensions and the Chrome Web Store. There is one actions
model per window, so under the shell Chrome doesn't make its own
(`ToolbarManager` patch). `compass_apk` needs `is_desktop_android`.

The Web Store offers installs only to a desktop browser, so the shell gives
chromewebstore.google.com a "Desktop site" exception the first time it runs
(`Extensions`); the user can change it in site settings. The install prompt
is a tab-modal dialog, like a page's `alert()`: Chrome shows those only
while its browser controls are fully shown, and under the shell the
controls that count are the bottom ones, where Compass's bar is
(`ChromeTabModalPresenter` patch).

## Sync and push

- **Sync.** Sync is the desktop's, unchanged (sync.md). The Android client
  reports `OS_TYPE_ANDROID` and its form factor through Chromium's own
  DeviceInfo code.
- **Push.** `server/push` gains an FCM sender (`fcm.go`, FCM HTTP v1 with a
  service account) next to the APNs one.
  - Registration takes `platform: "apns" | "fcm"`, with APNs as the default
    so current iOS clients keep working.
  - The client is `CompassPushAndroid` (`browser/sync`), started with the
    sync notifier. It gets an FCM token for Compass's Firebase project (its
    sender ID) through Chromium's own Instance ID driver, which uses Play
    services, so no Firebase library is added. It registers the token as
    iOS registers its APNs token (`POST <server>/compass/push`,
    `platform: "fcm"`), and a wake-up (a message to its Instance ID app)
    refreshes SEND_TAB_TO_SELF. Chrome's send-tab-to-self then shows the
    tab's notification. A device that leaves the chain removes its
    registration (`DELETE <server>/compass/push`), as iOS does.
  - The message carries nothing but `type: "send-tab"`: the tab comes by
    sync, encrypted, as on iOS (send-tab.md).
  - The server needs the project's service account key
    (`COMPASS_SYNC_FCM_CREDENTIALS`); it lives with the server's
    configuration, never in this repository.

## Building

- **Checkout.** A checkout builds both desktop and Android once `.gclient`
  says `target_os = ['mac', 'android']`. Android builds go in
  `out/<Config>-android-arm64`.
- **Host.** Chromium supports Android builds only on Linux. Compass also
  builds them on Apple silicon Macs: building.md, "Android from a Mac".
- **Configs.**
  - `portable` (non-component release, no symbols) makes test APKs.
  - `release` (`is_official_build`) makes the Play bundle and the GitHub
    APK.
  - Test APKs are large, about 500 MB unstripped. Release builds are far
    smaller.
- **Desktop-only code.** Patches that add `//compass/browser/*` to targets
  Android also builds gate it with `if (!is_android)`, and their C++ hooks
  with `#if !BUILDFLAG(IS_ANDROID)`. Inside `//compass`, a BUILD file that
  Android loads keeps its desktop targets under `if (!is_android)`, because
  GN resolves every target of a file it loads. Each milestone ungates what
  it brings to Android. Today Android gets `components/sync`, the protocol,
  strings, `common`, `browser:url_handler`, `browser:feature_overrides`,
  `browser/prefs` and `browser/updates:hooks`.
- **Signing.** The upload keystore and the FCM service account are kept out
  of the repository.

## Milestones

| # | Scope | State |
|---|---|---|
| C0 | Tooling: `--cpu android-arm64`, `config/args/android.gn`, the Mac-host patches and fix-ups, and a stock Chromium build from the Compass checkout with every patch applied; desktop-only code gated off | Done |
| C1 | The app and the shell spike: `//compass/android:compass_apk` (package, name, icon) and `CompassActivity`, with a Compose bottom bar over Chrome's tabs and compositor | Done; the shell lives in Chrome's tabbed activity (`CompassShell`) rather than an activity of its own |
| C2 | SpaceModel bridge; sync joins a chain from a debug pairing screen | Done |
| C3 | Sidebar, spaces, favorites, pinned tree, Today and archive in Compose | Done; no archive view or drag and drop yet |
| C4 | Bottom-bar gestures, tab picker, command bar, theme editor, undo closing, incognito lock | Done (the bar scrolls away, a swipe on its address switches tabs; incognito lock and undo are Chrome's) |
| C5 | Pairing UI (QR code, code, recovery code), profiles, and Settings sections for Compass | Pairing by code, recovery code and QR code (from the camera) done; one profile; no Compose settings yet |
| C6 | Passwords and Autofill for other apps, history, sessions, devices; sent tabs over FCM (`server/push/fcm.go`) | Done; other apps fill but can't save into Compass |
| C7 | Extensions: Web Store installs, 1Password, uBlock, and the desktop's extension list | Web Store installs and the extensions sheet done (uBlock Origin Lite tested); 1Password not tried |
| C8 | Claude: chat sheet, page tools, browser tools | Done (page tools; list, open, select and close tabs); not yet tried with a real key |
| C9 | Widgets, Quick Settings tile, shortcuts, share target | Done |
| C10 | Tablets and foldables, keyboard shortcuts, multiple windows | Done |
| C11 | Release pipeline: official build, Play internal testing, APK on the GitHub release | Not started |

## Verification

- **Each milestone:** build the APK and install it on a Genymotion SaaS
  arm64 device (Pixel 9, Android 14) with gmsaas and adb, plus screenshots
  through adb.
- **Unit tests.** The C++ sync and model unit tests run in the Android
  build as well. The Kotlin bridges and UI get junit/Robolectric tests and
  Compose instrumentation tests.
- **End to end:**
  1. Pair with a desktop Compass by QR code.
  2. Spaces, the pinned tree, favorites and themes match both ways.
  3. Passwords fill in another app.
  4. A tab sent from the desktop shows a notification.
  5. 1Password installs from the Web Store and fills a login.
  6. Claude reads the page.
  7. Check the layout in a phone profile and a tablet profile.
- `go test ./server/push/...` with an FCM fake.

## Risks

- **Rebase cost.** `chrome/android` changes a lot upstream. Keeping the
  shell in `//compass` and hooking in at few points keeps updates
  manageable.
- **Desktop-Android extensions are still upstream work in progress.**
  Parts may be missing or change between versions.
- **Mac host.** Upstream calls it "best-effort", so a Chromium update can
  break it. The fix-ups are small and checked on every build, and a Linux
  host stays possible.
