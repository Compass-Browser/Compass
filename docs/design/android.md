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
  Jetpack Compose. Chromium (156 and 157) builds Kotlin and Compose itself
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
- **The archive (P1),** as on iOS and the desktop (sidebar.md, "Today
  auto-archive"), on the desktop's `SpaceModel::Archive()`. Closing a Today
  entry's tab sends the entry to its space's archive
  (`SpacesAndroid::TabClosed()`), however it closes: the row's ×, Close,
  "Archive" in the long-press menu, Today's Clear, a flick in the tab
  picker, or Chrome's own closes. Undo (the shell's snackbar) brings the
  entry back out of the archive, at the top of Today
  (`TabClosureUndone()`). An entry with no open tab is archived directly
  (`ArchiveNode()`). The archiver runs as on the desktop, 30 s after the
  spaces load and every 10 minutes after that: it archives the Today
  entries that went unseen for their space's delay and closes their tabs
  (`OnTabsArchived()`, no undo). An entry shown in a window is kept, and
  so is one whose tab plays sound (Chrome's tab models); each
  window's bridge reports the tab it shows, and both the tab a window
  leaves and the one it shows are stamped as last active, so the clock
  starts when an entry leaves the screen. The space's ⋯ menu has
  "Archive", a sheet of the space's archived entries, newest first, with
  their age (a tap puts one back at the top of Today and opens it, × drops
  one, Clear drops them all), and "Archive Tabs After…" (12 or 24 hours, 7
  or 30 days, or never: the space's `archive_after`, which syncs).
- **Pinned URL (P2)** (sidebar.md, "Pinned URL"). A pinned tab's or
  favorite's long-press menu, while it's on another page than its pinned
  URL, has "Use This Page as Pinned URL" (`SetSavedUrl()` with the page)
  and "Back to Pinned URL", which loads the pinned URL in its tab, or sets
  an unloaded entry's page back to it ("Favorite URL" for favorites).
  Closing one's tab takes it back to its pinned URL, unloaded
  (`SpacesAndroid::TabClosed()`).
- **Drag and drop (P5)** in the sidebar (`SidebarUi.kt`, `EntryDrag`): a
  long press on an entry picks it up (its menu opens as before, and goes
  once the finger moves); the entry follows the finger and drops by the
  row under its middle: above or below an entry, into a folder (its
  middle; not a live folder), at the top of Pinned or Today (their
  headers or New Tab), or into the favorites (the grid). A line or a fill
  shows where. `SpacesAndroid::MoveNodeTo()` moves it on the model's own
  rules (`CanMove()`, `Move()`; across spaces `MoveToSpace()`), with its
  tab. Not yet: dropping on another space's icon (the menu's "Move to"
  does that), dragging favorites or a split's panes, and scrolling the
  list while dragging.
- **On Launch (P3)** (`OnLaunch`), as on iOS: Settings → On Launch is
  **Sidebar** (the default: the sidebar comes up over the restored tab) or
  **Last Tab** (the tab last shown, sidebar down; if there's none once
  Chrome has restored its tabs, or it's on the start page or incognito,
  the sidebar). It's the first window's launch in the app's process, and
  only for a launch from the launcher: a link from another app, or
  Compass's tile, widget and shortcuts, show what they open. The tab last
  shown is the one Chrome restores as selected, and the space is the last
  one a window switched to (the list's active space, which the sidebar
  file keeps), so a launch opens on it. The setting is an app preference
  (`compass_on_launch`), not synced, as on iOS.
- **Sync (C2, C5).** `CompassSyncService` runs on Android as on the
  desktop (chain, pairing, Nigori, the notifier), without what needs
  windows: the refresh on a window's activation becomes the app coming
  back (`ApplicationStatusListener`). COMPASS_SPACES runs the desktop's bridge on
  `SpacesAndroid` (`compass_spaces_sync_android.cc`): an entry is in use
  while it has a tab. The sync screen (`SyncScreen`, from the sidebar's
  Sync row) joins a chain with a pairing code or the recovery code,
  starts one, adds devices and approves them, and turns data types on
  and off, through `SyncBridge` (the desktop's `SyncController`). A
  desktop's pairing QR code (`compass-pair:?server=…&code=…`, sync.md)
  opens in Compass from the camera (`PairInviteActivity`, which registers
  the scheme) and joins with it.
- **Sync's transport data.** Android's `SyncServiceImpl` prunes per-account
  sync prefs to the OS's Google accounts (at startup and when they change);
  local sync has no account, so its cache GUID and birthday are keyed to the
  empty Gaia ID and were dropped, and every launch synced as a new client
  (a new device each time, every type's data downloaded again). The
  `components-sync-android-sync_service_android_bridge.cc` patch keeps the
  empty Gaia ID's entry. Together with the spaces bridge starting once the
  spaces load (`compass_spaces_sync_android.cc`), a paired Pixel 8a gets the
  chain's spaces, favorites and profile list.
- **Profiles (P7), phase 1.** As on the desktop (sync.md, "Profiles"),
  every profile of the chain is a Chromium profile with its own sync
  account: the home's COMPASS_SPACES bridge carries the chain's profile
  list (`GetLocalProfiles()`, `OnChainProfiles()`), and
  `CompassSyncService` creates the ones this device doesn't have
  (`CompassProfileCreator`). The desktop's `SpaceDirectory` keeps every
  profile's spaces in one order; on Android it's built with
  `SpacesAndroid` in place of `SpaceService` (`//compass/browser/android:spaces`),
  which reports to it as it's created, loaded and destroyed. The snapshot
  lists every profile's spaces (`own` for the window's profile, `profile`
  its name, `profile_count`); the sidebar's row shows them all with a
  wider gap where the profile changes, and the space header names the
  profile when there's more than one. `SidebarState.spaces` stays the
  window's profile's (moves, swipes, shortcuts), `allSpaces` is the row.
  - **A second Chromium profile on Android.** Chrome on Android has one
    profile, made from `StartupData`. Three patches let the chain's other
    profiles load:
    - `ProfileImpl` takes the startup data's prefs only for the profile it
      was built for; another profile loads its own
      (`LoadPrefsForNormalStartup()`) and gets a proto database provider on
      its key at once (services built with it, such as the image fetcher
      GCM's factory asks for, need it), which its storage partition takes
      later (`TakeDefaultProtoDatabaseProvider()`).
    - `GCMDriver.create()` makes a second driver instead of throwing; the
      first profile's stays the one messages go to (a later profile gets no
      web push or GCM invalidations; Compass's own push is the home's).
    - `ChromeBrowserMainPartsAndroid::PostProfileInit()` runs only the
      common per-profile part for a later profile.
    With them, Android creates the chain's profiles (`ReconcileProfiles()`),
    loads the listed ones (`SpaceDirectory::LoadListedProfiles()`) and lists
    the chain's profiles it has but doesn't list (`ListChainProfiles()`). A
    profile with spaces is listed as it gets them (`SpacesAndroid::Get()`),
    and the profile creator writes the directory's file at once. Tried on a
    Pixel 8a with a three-profile chain: each profile's spaces sync in its
    own account, and tapping another profile's space opens it in that
    profile's window.
- **Profiles, phase 2: a window per profile.** Chrome on Android gives a
  window one regular profile (`ActivityProfileProvider`:
  `getLastUsedRegularProfile()`) and two tab models, so a window shows one
  profile's spaces, and showing another profile's space swaps windows, as
  the desktop swaps a window's view (profiles-in-place.md):
  - `ChromeTabbedActivity.createProfileProvider()` asks
    `CompassShellHook.createProfileProvider()` first (patch); the window
    has its ID by then. `WindowProfiles` keeps each window's profile (its
    directory's name, by window ID, in app preferences) and provides it,
    with its primary OTR profile for incognito, as soon as the profile
    manager is ready (`ProfileManager.Observer`, as
    `ActivityProfileProvider`; a supplier's callbacks are posted, and a
    window restored at launch asks for its profile before they'd run). A
    window made for a profile, or relaunched for it, loads it then and
    there (`ProfilesBridge.loadProfileSync()`: `ProfileManager::GetProfile()`,
    only an existing profile in the user data directory); any other window
    takes the last used profile and keeps it (Chrome's own provider follows
    the last used profile, which another profile's window changes). A
    profile that's gone gives the window the last used one.
  - Tapping another profile's space (`Spaces.onSpaceClicked()`) shows it
    in a window of that profile: a running one comes forward
    (`AppTask.moveToFront()`) and shows the space itself
    (`CompassBar.handleIntent()`, since Chrome makes a new task for a
    window it didn't start), else `ACTION_OPEN_SPACE` with the profile's
    directory goes to one closed with its tabs (through
    `MultiWindowUtils.launchIntentInMaybeClosedWindow()`), or opens a new
    window for it as Chrome's own new windows, without animation. The
    window left behind leaves Recents (`AppTask.setExcludeFromRecents()`)
    until it's resumed again, so Recents shows the window in use.
  - The shell's own parts already take the window's tab model's profile
    (spaces, bar, settings, sync, history, extensions, Claude), so they
    follow. Chrome's own code that reads `getLastUsedRegularProfile()`
    (about 90 files: downloads, notifications, some settings) still acts
    on that profile; `PasswordSaveActivity` (saves from other apps) too.
- **Profiles, phase 3.** A space's menu has "Move to <profile>" for each
  other profile of the chain (not for the window's last space), after a
  confirmation that its tabs leave their sites' sign-ins behind.
  `SpacesBridge.moveSpaceToProfile()` is the desktop's
  `MoveSpaceToProfile()` (spaces.md, "Assigning a profile"): a space of
  the target profile takes its ID, name, icon, theme, archive delay,
  entries and archive, and the old one goes. Open tabs can't change
  profile, so the window closes them (`SpacesAndroid::ReleaseTabsOf()`
  hands them back first, so the entries stay) and shows the space in the
  target profile's window. Settings has a Profiles section when the chain
  has more than one: each profile's name, its spaces, and which one this
  window shows. Tried on the Pixel 8a: switching between profiles keeps
  two windows, a cold start with both restored works, and a space moved
  to another profile shows up there.
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
  splits, history, downloads) aren't offered. The header has the model
  menu (the desktop's known models, `compass.claude.model`), and Claude
  keeps memories as on the desktop (claude.md, "Memory"): the memory
  tools, a "Saved to memory" line with Undo, and Memory in the sheet's ⋯
  menu to see, add and delete them; they sync with the spaces.
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
  Android's setting to choose Compass. Saving from other apps (P10): the
  fill response carries a `SaveInfo` (password, and the username if there
  is one) when Compass can tell whose login it is: a web view's domain
  (`https://<domain>/`) or an app's signing certificate
  (`android://<SHA-512, URL-safe base64>@<package>/`, Chrome's form for app
  logins, affiliation_utils.h). When the user picks Save in Android's
  prompt, `PasswordSaveActivity` starts the browser if needed and
  `PasswordsBridge::Save()` adds the login (replacing one with the same
  site and username). The picker lists an app's own saved logins first.
- **Extras (C9),** in `it.cipi1965.compass.extras` and the manifest's
  `extra_application_definitions`: a Quick Settings tile (new tab with the
  command bar), widgets (search; Quick Actions: search, Incognito, Claude,
  the sidebar; Today: the active space's first four Today entries, which
  `Spaces` publishes to `TodayWidgetProvider` as they change, so it shows
  them while Compass isn't running; Other Devices: the sync chain's other
  devices' open tabs, which `DevicesWidget` publishes from Chrome's
  `ForeignSessionHelper`; both through `ListWidgetProvider`), a "Pin to
  Compass" share target, and a
  launcher shortcut per space (dynamic, from `Spaces`). They reach the
  shell through `CompassIntents` (its own actions to Chrome's launcher
  alias), which `CompassBar` handles at launch and on new intents.
  Pin to Compass opens the shared link and pins it to the active space,
  then a sheet (`PinPickerUi.kt`, as the iOS share extension's picker)
  moves it to the favorites, another space's pinned tree or one of its
  folders (live folders aside; the snapshot lists each space's
  `pinned_id` and `folders`).
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
  `AutocompleteController`, and tabs on other devices (command-bar.md:
  sync's open tabs through Chrome's `ForeignSessionHelper`, every word in
  the title, URL or device name, at most six, the most recently active
  device first; one opens as a new Today entry).
- **History** (`HistoryScreen`, `HistoryUi.kt`), from the page menu, in
  place of Chrome's: as the iOS app's, a search field over the visits
  (Chrome's `BrowsingHistoryBridge`, a page at a time as the list scrolls),
  newest first under Today, Yesterday and their dates; a tap opens one in
  the tab, × removes it, Clear… opens Chrome's Clear Browsing Data.
- **Downloads** (`DownloadsScreen`, `DownloadsUi.kt`), from the page
  menu, in place of Chrome's download home: the profile's downloads from
  Chrome's `OfflineContentProvider` (`OfflineContentAggregatorFactory`),
  newest first, kept up to date while open; those under way show their
  progress with Pause or Resume and ×; a tap opens a finished one, a long
  press deletes it (after asking). Settings opens Chrome's download
  settings.
- **The start page** (`StartPage`, `StartPageUi.kt`) replaces Chrome's new
  tab page (`NativePageFactory` patch): as the iOS app's empty page, the
  space's tint; for an incognito tab, a dark page with what incognito
  keeps. As on the
  desktop's empty tab, a tab on it has no sidebar entry until it goes
  somewhere (`SpacesAndroid`).
- **Look.** The shell has the iOS app's layout (ios.md) in Material You's
  style (`CompassStyle.kt`): the dynamic color scheme (`CompassTheme`),
  tonal surfaces and Material 3 shapes and components (list items, tonal
  buttons, menus with leading icons, a snackbar for undo, a FAB), with the
  space's tint behind the sidebar, the picker and the start page
  (`spaceTint`) and site icons from Chrome's favicon database (`Favicons`,
  `SiteIcon`). `CompassTheme` provides `onSurface` as the content color:
  the shell draws its own backgrounds rather than sitting in a `Surface`,
  where text without a color of its own would be black.
  - **Space themes** (spaces.md, "Gradients", "Intensity and grain",
    "Theme editor"; the iOS app's `SpaceTheme`, ported in `SpaceTheme.kt`):
    the snapshot carries each space's `colors` (the color, then the
    gradient), `intensity`, `grain` and `appearance`
    (`SpacesAndroid::Snapshot()`), and `SpacesBridge::SetSpaceTheme()` sets
    them all. `SpaceThemed()` puts the tint on the backdrop of the theme's
    mode (automatic follows the system unless the tint decides) and picks
    the text's mode by contrast, as the desktop's `GetTintColorMode()` and
    `GetTintTextMode()`; `spaceTint()` paints the three stops from the top
    left to the bottom right at the intensity's opacity, with the desktop's
    grain tile over them (overlay), fading over 280 ms. The sidebar (sheet
    and pane), the start page, the tab picker and the command bar use them.
    Above the default intensity, Material's `primary`, `onSurfaceVariant`
    and outlines move toward the text's color in proportion (all the way at
    full opacity): Material made them for its own surfaces, and on a strong
    tint secondary text fell to about 2:1.
  - **The theme editor** (`ThemeEditorUi.kt`, from the space's ⋯ menu,
    after iOS's `ThemeEditor`): a full-height sheet in the theme being
    edited, with the mode (automatic, light, dark), the pad (hue round the
    center, saturation out; the big dot is the color and takes the
    gradient along), − and + for one to three colors, the palette and a
    lightness slider (with the pad, any color: Android has no system color
    picker), the gradients (`SpaceTheme.PRESETS`), intensity and grain, and
    the space's emoji. What changes goes to the space at most every 50 ms
    and when the sheet closes. Renaming keeps a symbol icon picked on
    another device (`SpacesBridge::EditSpace()`); symbols themselves don't
    show on Android yet.
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
    Clear show a Material snackbar over the bar ("Closed “Title”", Undo;
    four seconds, or a flick down) instead of Chrome's
    (`UndoToast`, `UndoToastUi.kt`): a closure the tab model holds pending
    shows it, Undo cancels the closure and its end commits it; the
    `UndoBarController` patch keeps Chrome's snackbar away.
    A Today tab brought back gets its entry back from the archive (P1).
  - **Settings** (`CompassSettings`, `CompassSettingsUi.kt`), from the
    sidebar's gear and the page menu, a Material settings screen: Sync, then the Chrome settings pages
    Compass keeps, each opened at its page (passwords, clear browsing data,
    site settings, privacy, …), and Passwords in Other Apps, Claude and
    Extensions. Search Engine is Compass's own page (P8): the engines
    Chrome offers (`TemplateUrlService`: its prepopulated ones, and the
    default if it's another), the default checked; a tap makes one the
    default (`setSearchEngine()`), unless policy set it. Privacy and
    Security is Compass's own page too: Safe Browsing's level (Enhanced,
    Standard, None; `SafeBrowsingBridge`), and switches for blocking
    third-party cookies (`profile.cookie_controls_mode`: block third-party,
    or Chrome's default of Incognito only), always using secure connections,
    preloading pages (`PreloadPagesSettingsBridge`: standard or none) and
    Do Not Track; what policy sets is shown but can't be changed. Clear
    Browsing Data and More Privacy Settings open Chrome's pages. Site
    Settings is Compass's own as well: each permission's default for every
    site (location, camera, microphone, notifications, pop-ups, automatic
    downloads, sound, JavaScript; `WebsitePreferenceBridge`'s categories:
    on asks first or allows, off blocks), and All Sites and Exceptions on
    Chrome's page.
  - **Default Browser**, the first row of Browsing, says whether Compass
    is Android's browser app (`RoleManager.ROLE_BROWSER`) and asks for it
    with Android's own dialog (`createRequestRoleIntent()`, as Chrome's
    `DefaultBrowserPromoManager`). When Compass already is the browser app,
    the row opens Android's Default apps settings
    (`ACTION_MANAGE_DEFAULT_APPS_SETTINGS`) instead, to pick another; so
    does an answer in under half a second, which isn't the user's (a
    dialog Android didn't show, after "Don't ask again" on versions that
    honor it; Android 16 in the emulator showed the dialog again). The row
    reads the role again when the window comes back. Tried in the
    emulator: set, cancelled, and already set. Chrome's own startup promo (from the third
    launch, `TabbedRootUiCoordinator`) is upstream's and may still show.
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
| Space themes and editor | A Compose editor (`ThemeEditorUi.kt`) with the mode, pad, palette, gradients, intensity and grain; the theme tints the sidebar, the picker and the start page and picks their light or dark mode |
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
- **Signing.** The upload keystore, the Play service account and the FCM
  service account are kept out of the repository. Play holds the app
  signing key (Play App Signing); the build is signed with the upload key
  (building.md, "Android releases (Google Play)").

## Milestones

| # | Scope | State |
|---|---|---|
| C0 | Tooling: `--cpu android-arm64`, `config/args/android.gn`, the Mac-host patches and fix-ups, and a stock Chromium build from the Compass checkout with every patch applied; desktop-only code gated off | Done |
| C1 | The app and the shell spike: `//compass/android:compass_apk` (package, name, icon) and `CompassActivity`, with a Compose bottom bar over Chrome's tabs and compositor | Done; the shell lives in Chrome's tabbed activity (`CompassShell`) rather than an activity of its own |
| C2 | SpaceModel bridge; sync joins a chain from a debug pairing screen | Done |
| C3 | Sidebar, spaces, favorites, pinned tree, Today and archive in Compose | Done |
| C4 | Bottom-bar gestures, tab picker, command bar, theme editor, undo closing, incognito lock | Done (the bar scrolls away, a swipe on its address switches tabs; incognito lock and undo are Chrome's) |
| C5 | Pairing UI (QR code, code, recovery code), profiles, and Settings sections for Compass | Done: pairing by code, recovery code and QR code (from the camera); profiles (P7); Compass's own Settings screens (P8) |
| C6 | Passwords and Autofill for other apps, history, sessions, devices; sent tabs over FCM (`server/push/fcm.go`) | Done; other apps fill but can't save into Compass |
| C7 | Extensions: Web Store installs, 1Password, uBlock, and the desktop's extension list | Web Store installs and the extensions sheet done (uBlock Origin Lite tested); 1Password not tried |
| C8 | Claude: chat sheet, page tools, browser tools | Done (page tools; list, open, select and close tabs); not yet tried with a real key |
| C9 | Widgets, Quick Settings tile, shortcuts, share target | Done |
| C10 | Tablets and foldables, keyboard shortcuts, multiple windows | Done |
| C11 | Release pipeline: official build, Play testing tracks, APK on the GitHub release | Working on it: the app bundle (`compass_bundle`), versionCodes from the release version, `compass play upload` to a track, and the listing and App content drafts in `android/play/` (building.md, "Android releases (Google Play)"); on internal testing since 2026-11-18, closed testing in Google's review. Not yet: the APK on the GitHub release, and `compass play upload` (no Play service account yet) |

## Parity with iOS

What the iOS app (ios.md) does that Android doesn't yet, in the order
planned. The first group is mostly shell work; splits and Claude in the
sidebar are the large pieces left.

| # | Scope | Notes |
|---|---|---|
| P1 | Archive | **Done** (above, "The archive"). Closing a Today entry sends it to the space's archive; "Archive" in the sidebar's long-press menu; an archive view from the space menu (sidebar.md's rules, as on the desktop) |
| P2 | Pinned URL | **Done** (above, "Pinned URL"). "Use This Page as Pinned URL" and "Back to Pinned URL" for a pinned tab or favorite; closing one goes back to its pinned URL |
| P3 | On Launch | **Done** (above, "On Launch"). Settings → On Launch: Sidebar (default) or Last Tab; the last shown tab and space are kept as they change, and a launch opens on the last space |
| P4 | Tab picker gesture | **Done** (C4: `CompassBarUi.kt`, "The bar"; the picker opens when the swipe ends rather than following the finger). Swiping up on the bar left of the address pill opens the tab picker, as on iOS (pill: sidebar; right: command bar) |
| P5 | Sidebar drag and drop | **Done**, within the shown space and into the favorites (above, "Drag and drop"); onto other spaces through the menu. Reorder entries and move them between folders, Pinned and Today, and onto other spaces |
| P6 | Splits | "Split with…" and "Add to split" in the long-press menu; a split of two shows both panes on the phone (stacked, side by side in landscape), three or four one pane at a time under a pane picker |
| P7 | Profiles | **Done** (above, "Profiles", phases 1–3). Every profile of the chain synced in its own account, as iOS M8 and the desktop do; their spaces in one sidebar, favorites per profile |
| P8 | Compass settings | **Done** (above, "Settings"): search engine, privacy, site settings; a site's own exceptions and the rest on Chrome's pages. Compass's own Settings screens for what's still Chrome's: search engine, privacy, site settings; Chrome's screens stay for the rest |
| P9 | System extras | The share target picks a space and folder (done, above, "Extras"). Today, Quick Actions and Other Devices widgets (done, above); app search for favorites and Pinned entries (Spotlight's role); a space per Do Not Disturb mode or work profile if Android allows it (the Focus filter's role) |
| P10 | Passwords from other apps | **Done** (above, "Passwords in other apps"). Other apps' logins save into Compass (Autofill's save request), not only fill |
| P11 | Link folders | A link opened in the background gathers with its page in a Today folder, by the desktop's rules (sidebar.md, "Link folders"; ios.md, "Link folders") |
| P12 | Claude in the sidebar | Names for link folders, Tidy Today, shorter tab titles, Ask in the command bar (ai-sidebar.md, A2–A5; ios.md); needs P11 for the names |
| P13 | Links routed to spaces | A web link from another app on a domain with a rule opens as a new Today tab of the rule's space (link-routing.md; ios.md, "Links routed to spaces"); needs P15 for links without a rule |
| P14 | Peek | Long-press a link to look at it in a card over the page without opening a tab (peek.md; ios.md, "Peek") |
| P15 | Little windows | With Compass as the default browser, a web link from another app opens in Peek's card over what the window shows, not among the tabs (little-window.md; ios.md, "Little windows"); needs P14 |
| P16 | Reader mode | The desktop's Reader on the shared package (`common/reader/`) for a page that reads as an article (reader-mode.md; ios.md, "Reader mode") |
| P17 | Auto picture-in-picture | As on the desktop (picture-in-picture.md): leaving a tab whose video plays with sound, or leaving the app, shows the video in picture-in-picture; a setting turns it off (ios.md, "Auto picture-in-picture"). Chromium's own PiP didn't start in the emulator |

Profiles, still the last used profile's: Chrome's own code that reads
`getLastUsedRegularProfile()` (downloads, notifications, some settings),
and saves from other apps (`PasswordSaveActivity`), act on the last used
profile rather than the window's; only the first profile gets web push.

Still Chrome's look, to restyle in Material You with the shell's layout:
a link's long-press menu, page info and permission prompts, and the "Save
password?" message. Chrome's own top toolbar also
shows for a few seconds on a cold start before the shell hides it.

Built but not yet tried end to end: the Claude tool loop with a real key,
pairing with a desktop by QR code or code, a sent tab arriving by FCM,
autofill in another app with saved passwords, and 1Password from the Web
Store.

## Verification

- **Each milestone:** build the APK and install it on a Genymotion SaaS
  arm64 device (Pixel 9, Android 14) with gmsaas and adb, plus screenshots
  through adb.
- **Unit tests.** The C++ sync and model unit tests run in the Android
  build as well. The shell's state and logic have junit/Robolectric tests
  on the host's JVM (`//compass/android:compass_junit_tests`, `android/junit/`:
  the sidebar's snapshot, drops, On Launch, the Claude sheet's state;
  `out/<dir>/bin/run_compass_junit_tests`). Compose instrumentation tests
  are still to come.
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
