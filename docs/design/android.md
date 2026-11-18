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
- **Link folders (P11)** (sidebar.md, "Link folders"), the desktop's
  rules: a link opened in the background from a page in the sidebar (the
  link's long-press "Open in new tab", Ctrl+click or middle click with a
  keyboard: Chrome's `FROM_LONGPRESS_BACKGROUND` launch types, not
  selected) gathers with that page in a Today folder. The shell passes the
  tab's parent with it (`Spaces.linkOpener()`, `SpacesBridge.tabOpened()`),
  and `SpacesAndroid::TabOpened()` places it with the window's
  `LinkFolders` (`browser/sidebar/link_folders.h`, the desktop's rules for
  clients that add a link's entry themselves; one per window, not saved).
  An opener loose in Today gets a folder in its place (a split goes in
  whole), one in a Today folder adds the link there, and a pinned entry,
  favorite or another space's entry gets a folder at the top of Today,
  reused while it's there. Inside it, a link goes after its opener's
  previous link, else right after the opener. A link whose tab has no page
  yet keeps its opener until it does (`link_openers_`). The folders are
  marked as this device's (`MarkLinkFolder()`), so "Name Link Folders"
  names them (P12). Tests: `LinkFoldersTest`.
- **The link menu** (`LinkMenu`, `LinkMenuUi.kt`): a long press on a
  link, an image or the page opens Compass's sheet in place of Chrome's
  dialog, as iOS's context menu: the link (or the image's thumbnail) at the
  top, then its items in cards, each with its Material Symbol. Chrome's
  populator still builds the items and runs them
  (`ContextMenuHelper` asks `CompassShell.createContextMenu()` for the menu
  and hands it the items; a mouse's or a highlight's menu stays Chrome's
  popup), so its actions, the extensions' items and their submenus all
  work. On a link: Peek, **Open in New Tab** (Compass's: a new tab, shown),
  **Open in Background** (Chrome's "Open in new tab", which opens in the
  background and joins the page's link folder), Open in Incognito, **Open
  in Little Window** and **Pin** (Compass's: a new Pinned entry of the
  shown space, without a link folder), Open in New Window; then Copy Link,
  Copy Link Text, Download Link and Share Link in a card of their own; an
  image's items after them ("Preview image" is Peek Image: Peek's card). In
  an incognito tab Little Window and Pin aren't offered. Tab groups' and the
  reading list's items are left out.
- **The Site sheet** (`SiteSheet`, `SiteSheetUi.kt`): the page menu's
  Site Settings opens Compass's sheet in place of Chrome's page info,
  which drops from the top toolbar Compass doesn't have. It shows the site
  and its connection (secure, not secure, dangerous:
  `SecurityStateModel`), the permissions the site was given or denied (a
  switch each, and Reset Permissions; a permission still at "ask" isn't
  listed), the site's own settings as iOS's Site Settings has them
  (Desktop Site, which reloads the page, Sound, Pop-ups and Redirects,
  Automatic Picture-in-Picture, JavaScript), and All Site Settings,
  Chrome's page for the site (stored data, every permission). It reads and
  writes through `WebsitePreferenceBridge`. Compass's own pages and
  chrome:// pages still get Chrome's page info.
- **A long press inside Peek's card** opens the link menu too (Chrome's
  populator for a page without a tab, `ContextMenuMode.THIN_WEB_VIEW`, with
  a `ThinWebViewContextMenuItemDelegate` naming the window's activity):
  Open in Background, Open in Incognito and Open in New Window open tabs of
  the window (`Peek.CardLinkOpener`), **Peek** loads the link in the card
  itself, and the copy, download and share items are Chrome's.
- **Filling in Peek's card**: the keyboard's accessory (passwords,
  addresses, cards) keys its state to the window's current page
  (`getCurrentWebContents()`), so suggestions for the card's page never
  showed. While the card shows, `Peek` hands it the card's page
  (`ManualFillingComponent.CompassPage`, which `ManualFillingMediator`
  asks first). Not tried yet: the emulator has no saved logins to offer.
- **Messages** (Chrome's banners: "Save password?", "Pop-up blocked",
  downloads, ...): they rise from the bottom of the window to just above
  Compass's bar, as iOS's save banner, rather than drop from the top, and a
  swipe down dismisses them. Chrome still queues, times and runs them
  (`MessageContainer.setFromBottom()`, set by `MessageContainerCoordinator`
  under the shell): the container sits at the bottom, over what the bar
  covers of the window (`DisplayCutoutController.getCompassBottomInset()`,
  and its observer as the bar shows and hides); the banners' translation and
  the swipe are mirrored, and a banner unfolds upward.
- **Dialogs**: tab-modal dialogs (permission prompts, a page's `alert()`,
  the extension install prompt) dock at the bottom of the window as a card,
  12dp off its edges, over the bar as Compass's sheets are, rather than in
  the middle (`TabModalPresenter.setCompassBottomDock()`, set by the bar).
  Their contents stay Chrome's (Material 3 already, with the precise or
  approximate location choice).
- **Peek (P14)** (peek.md; ios.md, "Peek"; `Peek`, `PeekUi.kt`): a
  link's page in a card over the window, without a tab.
  - **Long-press a link:** Chrome's menu starts with **Peek**, which
    shows the link in the card (the "Preview page" item, renamed and moved
    to the top; Chrome's preview page is left out of a Compass window's
    menu). Chrome's other items stay.
  - **Links from pinned tabs and favorites** that open a new tab
    (`target=_blank`, `window.open()` on a tap) peek too, as on the desktop
    and iOS: `addNewContents()` offers the new WebContents to the shell first
    (a foreground tab with a user gesture, from a tab that's a pinned entry
    or favorite), so the page keeps its opener.
  - **The card** is a sheet up from the bottom on a phone (to just under the
    status bar) and a centered card in a wide window (840dp and up), over
    the dimmed window. Its bar has a grabber, the page's icon, title (only
    its host until it has one) and host, **Open** and ×; a bar under it shows
    loading.
  - **Close:** ×, back, Esc, Ctrl+W, a tap on the dim, dragging the bar
    down, `window.close()`, or another tab shown. The page goes.
  - **Open** (Ctrl+O): the page becomes the window's active tab, a new
    Today entry of its space, with its history, scroll position and form
    state: `ChromeTabCreator.createTabWithWebContents()` adopts the same
    WebContents, as Chrome does for a page's popups.
  - Inside the card, links load in the card; one that opens a new tab or
    window opens a tab of the window (from its URL), and the card closes.
  - How: the page is a WebContents of the source tab's profile (incognito
    too) in a `ThinWebView`, as Chrome's preview page
    (`EphemeralTabCoordinator`): a view with its own compositor, which
    attaches the tab helpers a tab has (`TabHelpers::AttachTabHelpers()`,
    permission prompts on), so a tab adopting it has them. Open moves the
    page to the window's compositor and the ThinWebView goes after.
    ThinWebView already makes the page a tab for the performance manager,
    which the adopting tab's `TabFeatures` would do again (a `CHECK`, seen
    in the emulator) unless the page has the popup blocker it attaches with
    it; `PeekBridge.prepareForTab()` (`peek_bridge.cc`) gives the card's
    page that helper as it comes in.
  - Tried in the emulator: a long press's Peek, × and Open; a link from
    another app at a cold start and with Compass running, Open in Home (a
    Today entry, no reload) and back to close. Not yet tried: a pinned
    tab's links, the ⌄ menu, a wide window.
- **Little windows (P15)** (little-window.md; ios.md, "Little windows"):
  with Compass as the default browser, a web link from another app opens
  in Peek's card over what the window shows, not among the tabs.
  - `maybeHandleUrlIntent()` asks the shell first
    (`openLinkFromOtherApp()`, `LittleWindow.linkFromOtherApp()`): a VIEW
    intent for an http(s) page that Chrome would open in a new tab, not
    Compass's own intents or Chrome's trusted ones (its other windows,
    Custom Tabs' "Open in Compass"), not its widgets or shortcuts, not for
    incognito. Apps that open links in a Custom Tab keep it: that's their
    own little window.
  - At a cold start the link comes before the shell; it waits for it, and
    Chrome restores the tabs as for a plain launch, under the card. The
    sidebar doesn't come up (On Launch is for the launcher's launches).
  - The page is of the window's regular profile (never incognito). The bar
    has ×, back, the address (the title and host; a tap or Ctrl+L edits the
    URL, and Go loads what's typed, an address or else a search, in the
    card) and **Open in <space>**, naming the space shown, with ⌄ listing
    the window's profile's spaces when it has more than one (same profile
    only: the page can't change its website data). Either makes the page
    the newest Today entry of that space, as it is; another space is shown
    first. The bar takes the shown space's tint.
  - A second link replaces the first. Ctrl+[ (or Alt+←) goes back, Ctrl+R
    reloads, Ctrl+O opens.
  - **Settings → Browsing → Open in a Little Window** (on by default; the
    app preference `compass_little_window`): off, links from other apps
    open as tabs, as Chrome does.
  - Tests: `LittleWindowTest` (which intents, the setting).
- **Links routed to spaces (P13)** (link-routing.md; ios.md, "Links
  routed to spaces"): a web link from another app on a domain with a rule
  opens as a new Today tab of the rule's space, which the window shows,
  instead of the little window.
  - **Which links:** those the little window takes
    (`LittleWindow.linkFromOtherApp()`), whatever its setting. With no rule
    (or the rule's space gone), the little window, or with Open in a
    Little Window off a new Today tab of the shown space
    (`CompassBar.openLinkFromOtherApp()`). Never incognito.
  - **The rules** are the window's profile's, the desktop's `LinkRules` on
    its `SpaceList` (the sidebar file, synced with the spaces);
    `SpacesBridge` matches a link (`LinkRuleSpace()`: its host, then each
    parent domain), normalizes a domain (`NormalizeLinkRuleDomain()`), and
    sets, removes and lists them. At a cold start the link waits for the
    spaces to load (`Spaces.runWhenLoaded()`). Another profile's rules
    don't route a link that arrives in this window.
  - **The little window's ⌄ menu** has **Always Open <domain> in ›** for a
    page with a domain (so ⌄ shows with one space too): the profile's
    spaces, the rule's checked; picking one makes or changes the rule and
    opens the page there.
  - **Settings → Browsing → Sites in Spaces:** the rules by domain with
    their space (its emoji or initial on its color) or "Space deleted"; a
    tap changes the space or deletes the rule; Add Site takes a domain or a
    URL (a note when it isn't a site's domain, or what it will match).
  - Tried on a Pixel 8a: a link with no rule in the little window, Always
    Open example.com in Main, the next example.com link from another app
    straight to a Main tab, and the rule in Settings, deleted there.
- **Auto picture-in-picture (P17)** (picture-in-picture.md; ios.md, "Auto
  picture-in-picture"): leaving a tab whose video plays with sound shows
  the video in picture-in-picture, and coming back to the tab puts it back
  in the page. Upstream does it on Android behind
  `media::kAutoPictureInPictureAndroid` (`AutoPictureInPictureTabHelper`,
  made by `TabFeatures`, following the tab models), which
  `compass::RegisterFeatureOverrides()` turns on, with the desktop's
  browser-initiated auto picture-in-picture and the content setting allowed
  by default (picture-in-picture.md). The window is Chrome's
  `PictureInPictureActivity`. Settings → Site Settings → Automatic Picture
  in Picture turns it off (the content setting's default).
  - **Leaving the app, not yet.** The shell tells the tab helper when the
    window is left (`addOnUserLeaveHintListener`) and back
    (`PipBridge.setWindowLeft()`, `pip_bridge.cc`: the desktop's window
    hidden and shown again, `OnOcclusionStateChanged()`), and Chrome does
    start its picture-in-picture activity, but Android refuses it: Home
    stops the app's activity starts for a few seconds ("Background activity
    launch blocked! … appSwitchState: 0"), and the start comes after the
    page's round trip. Android's own way is the activity entering
    picture-in-picture as it's left (`setAutoEnterEnabled`), which takes
    the whole window, page and all; Chrome uses it only for a video already
    in fullscreen (`FullscreenVideoPictureInPictureController`). Its
    setting row ("When You Leave Compass", `compass.pip.on_leaving_window`)
    waits for it.
  - Tried on a Pixel 8a (Android 16): switching away from a playing video
    opens the window, and switching back closes it.
- **Reader mode (P16)** (reader-mode.md; ios.md, "Reader mode"): a page
  that reads as an article gets a book at the end of the bar's address
  pill; a tap shows the article over the tab in the desktop's reader page,
  and again goes back to the page, untouched underneath.
  - **The reader page** is the desktop's `chrome-untrusted://compass-reader`
    (`ReaderUntrustedUI`, its CSP and `ReaderArticleStore`), built for
    Android in `//compass/browser/android:reader` and registered by
    `compass::RegisterAndroidWebUIConfigs()` from the Android branch of
    `RegisterChromeWebUIConfigs()` (the `chrome_web_ui_configs.cc` patch;
    the desktop's registry is desktop-only). It's a WebContents of the
    tab's profile in a ThinWebView over the page and under the bar
    (`Reader`), so the look, the toolbar (×, Listen, Aa), the settings and
    their storage (the reader page's localStorage, per profile) are the
    desktop's.
  - **Is this an article?** `ReaderBridge.check()` (`reader_bridge.cc`)
    runs `Readability-readerable.js` and `reader.js` `check()` in the
    page's isolated world, as the desktop's `ReaderTabState`: when the
    shown tab finishes loading (and 2 s later), 1 s after a same-document
    navigation, and when a tab is shown that wasn't checked for its URL.
    `parse()` reads the article into the store and gives its token.
  - **Where:** the book in the pill (tinted while in Reader), Show/Hide
    Reader in the pill's long-press menu, a Reader tile in the page menu
    (any web page: one without an article shows the reader page's "no
    article" message), and Shift+Alt+R with a keyboard (the desktop's keys
    on Windows and Linux).
  - **Leaving:** the book or Shift+Alt+R again, the reader's × (its
    `window.close()`), Back, Esc, or a new document in the tab. A link in
    the article loads in the tab (which leaves Reader); one that opens a
    tab opens one. Reader is per tab: switching away takes the view away
    and coming back shows it again.
  - Tried on a Pixel 8a: Wikipedia's book, the article with Listen and Aa,
    ×, Back, and a link loading in the tab.
- **Splits (P6)** (ios.md, splits on the phone; sidebar.md, "Split tabs"):
  the desktop's split entries (two to four tabs, synced) open on the phone
  as on iOS. A split of two shows both panes, one above the other (side by
  side when the window is wider than tall), each under a title strip; the
  front pane is outlined in the space's accent and the bar acts on it; a
  tap on the other pane's strip brings it to the front. A split of three or
  four shows one pane at a time under a pane picker. `SplitView`,
  `SplitUi.kt`.
  - **Two pages at once.** Chrome draws one tab, the window's active one:
    the front pane. The other pane's page is borrowed into a ThinWebView
    (a compositor of its own, as Peek's and Reader's), with no delegate of
    its own, so it keeps its tab's, its helpers and its place in the tab
    model; it takes touches and scrolls by itself. The space it takes is
    Chrome's browser controls': a `COMPASS_SPLIT` top layer (the strips,
    and the top pane when the bottom one is in front) and `CompassBar`'s
    bottom layer (the bottom pane when the top one is), so Chrome's page
    ends at the pane; side by side, `CompositorViewHolder` leaves the other
    pane's width beside the page (`setCompassSideInsets()`). Only the bottom
    pane runs under the bar. Chrome's toolbar, which those controls would
    show, isn't drawn under the shell.
  - **Handing the page back.** The borrowed page goes back to the window
    (`setTopLevelNativeWindow()`) when its tab is about to show
    (`TabObserver.onShown()`, so before Chrome attaches its view), closes,
    or the split ends; its layer goes back into its tab's content layer
    (`SplitBridge.returnLayerToTab()`: the ThinWebView's compositor took it
    as its root, and the tab puts it there only once). A pane with no tab
    opens in a background tab (`Spaces.openInBackground()`), and shows once
    it has one; a tab restored frozen loads.
  - **The sidebar:** a split's row holds its panes side by side, the one on
    screen in bold; a tap on one opens the split with it in front. The
    long-press menu has, as on iOS, **Split With…** (a tab beside it),
    **Add to Split…** (a tab into a split beside it with room, or on a
    split a tab beside it), and on a split **Separate Tabs** (the desktop's
    `Unsplit()`), Pin, Archive, Move and Close, which archives it whole.
    Not in the favorites, live folders or incognito. `SpacesBridge.split()`
    and `unsplit()` are `SpaceModel::Split()` and `Unsplit()`.
  - **Resizing:** a handle on the line between two panes drags it (each
    pane 20% to 80% of the window); a double tap shares the window evenly
    again. The share is kept by split while the app runs, as the desktop's
    pane sizes (not saved).
  - **Dragging panes:** a long press on a pane's strip picks it up;
    dropped on the other pane, the two swap. In the picker, a long press
    on a segment picks it up and it drops at the place it's dragged to.
    Both are moves in the model (`Spaces.onMoveNodeTo()` into the split),
    so the sidebar's row and other devices follow.
  - **Other profiles:** a window shows one profile's spaces and its
    `SplitView` reads that window's model, so a split in another
    profile's space shows in that profile's window, as its entries do (not
    tried with two profiles).
  - Tried on the emulator: Split With and Add to Split, both panes stacked
    and side by side (beside the docked sidebar too), swapping the front
    pane both ways, three panes under the picker, Separate Tabs, and an
    archived split restored (its second pane opening in the background).
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
- **Claude in the sidebar (P12)** (ai-sidebar.md, A7): the desktop's
  services, `LinkFolderNamer`, `TodayTidier` and `ShortTitler`, build for
  Android as they are, over `SpacesAndroid` in place of `SpaceService`
  (`browser/ui/claude/sidebar_spaces.h`, the `claude_sidebar` target), on
  the desktop's `ClaudeQuick`, prompts and prefs. `ClaudeSidebarBridge`
  (one per window, on its regular profile) makes them and passes on what
  they do.
  - **Settings:** the sheet's ⋯ menu has Claude in the Sidebar: each
    feature's switch (off by default, and off until a key is saved) and its
    model (`ClaudeBridge::SetFeature()`, `SetFeatureModel()`; the snapshot's
    `features`).
  - **Names:** a folder's long-press menu (new: folders had none) has
    Rename (the user's name: `SetFolderName(kUser)`, so Claude doesn't name
    it again), and Undo Name while its name is Claude's, else Suggest a
    Name (a folder with entries, not a live one, the switch on). A name
    from Claude sparkles: the folder's icon turns to ✦ and the name fades
    in (`onFolderNamed()`). Link folders this device makes (P11) are named
    on their own at 3, 6 and 12 entries, as on the desktop.
  - **Tidy Today:** "Tidy" before Clear in Today's header, dimmed below 5
    loose entries, "Tidying…" while Claude answers. The result ("Tidied
    into N folders", with Undo, or why not) shows for 8 s: in the sidebar
    when it's open (on a phone it's a sheet, a window over the window's
    toast), else in the toast over the bar (`UndoToast.showMessage()`).
  - **Shorter tab titles:** the snapshot's tab rows carry
    `short_title`, which the row shows; `SpacesAndroid` passes on
    `OnNodeShortTitleChanged()`.
  - **Ask in the command bar:** a question (`LooksLikeQuestion()`, the
    browser's language) gets "Ask Claude" first, with "About this page"
    for a web page (its title and host). Return or a tap asks; the answer
    streams into a card in place of the results; typing goes back to them.
    Continue in Chat (or Return once answered) opens the sheet on a new
    chat that starts with the exchange (`ClaudeBridge::Seed()`).
  - Never in incognito. Tried in the emulator against a fake Claude
    server (each feature's request: titles and hosts only, the feature's
    model).
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
  launcher shortcut per space (dynamic, from `Spaces`).
  **App search** (Spotlight's role on iOS): after the spaces, the
  favorites and the shown space's Pinned entries are launcher shortcuts
  too, with their favicons, so the launcher's search finds them; one opens
  its entry (its tab, in its space, `Spaces.openEntry()`). Android caps an
  app's dynamic shortcuts (`getMaxShortcutCountPerActivity()`, usually 15,
  two of them Chrome's), so it's the first dozen or so in the sidebar's
  order; other spaces' Pinned entries aren't in the snapshot and aren't
  listed. They reach the
  shell through `CompassIntents` (its own actions to Chrome's launcher
  alias), which `CompassBar` handles at launch and on new intents.
  Pin to Compass opens the shared link and pins it to the active space,
  then a sheet (`PinPickerUi.kt`, as the iOS share extension's picker)
  moves it to the favorites, another space's pinned tree or one of its
  folders (live folders aside; the snapshot lists each space's
  `pinned_id` and `folders`).
- **The bar floats over the page**, as on iOS: the page runs under the
  capsule, with no strip behind it. It isn't one of Chrome's bottom
  controls, which the page would end above (the renderer's compositor
  clips the viewport to the controls' shown height whatever
  `controlsResizeView` says), but a view at the bottom of Chrome's
  coordinator, kept clear of the system's navigation bar from the window's
  root insets (Chrome's views take the insets before the bar would see
  them). It slides away as the page scrolls down and back as it scrolls up,
  at the top of the page, on another page and on another tab
  (`GestureStateListener.onVerticalScrollDirectionChanged()` on the shown
  tab's WebContents). The bar keeps a layer of no height in Chrome's stack
  (`COMPASS_BAR`), only so the strip Chrome paints under the navigation bar
  (its bottom chin) takes the bar's color.
  - **Safe area.** As in Safari, a page that asks for
    `viewport-fit=cover` gets the bar in its bottom safe area while the
    bar shows: `env(safe-area-inset-bottom)` is how much of the window the
    bar covers, and 0 (or the navigation bar's) while it's slid away, so a
    footer at `bottom: env(safe-area-inset-bottom)` sits above the bar and
    follows it. `CompassBar.pushSafeArea()` hands the value to Chrome's
    `DisplayCutoutController` (`setCompassBottomInset()`, a patch), which
    merges it into what it pushes to Blink (`setDisplayCutoutSafeArea()`).
    Other pages' fixed footers (no `viewport-fit=cover`, or no `env()`) sit
    under the bar while it shows. A page Peek or a little window handed to
    a tab gets the safe area from its next load: the tab never heard the
    page's `viewport-fit`.
  - **Scrolling.** `onVerticalScrollDirectionChanged()` and each scroll's
    start (`onScrollStarted()`'s direction) move the bar. A page Peek
    handed to a tab only has the second: its `GestureListenerManager`
    listens for direction changes on the card's view delegate, which the
    tab replaced. A fling's start isn't used: Chromium reads its direction
    from fields a fling doesn't fill (`gesture_listener_manager.cc`).
  - Tried in the emulator with a `viewport-fit=cover` page whose footer
    uses `env(safe-area-inset-bottom)`: 63 CSS px with the bar, 0 without;
    alerts still show.
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

A popup is a card apart from the page: rounded, 12dp off the window's edges
and 8dp off the bar. Its page lays itself out within that space: upstream
lets a popup's page grow to 800x600dp and then cuts the window to the
screen, which on a phone hid half of a wide popup like 1Password's. The
popup gives its page the window's width and the larger side of the bar,
less the margins, as the most it may take (`ExtensionActionPopupContents`'s
`setMaxSize()`). The card's corners clip the page, so it draws into a
TextureView rather than a SurfaceView.

**Unlock with biometrics** (`ExtensionUnlock`), as iOS's Face ID unlock
(docs/design/ios.md, "Web extensions"): desktop 1Password unlocks with its
desktop app's biometrics over native messaging, which nothing offers on a
phone. The same script as iOS's watches the extension's pages (its popup,
and its pages in the shown tab) for a visible password field. It runs in an
isolated world of the page (`ExtensionUnlockBridge`, only on
`chrome-extension://` pages), which can't post to the browser, so the
browser asks it for news every 400 ms while such a page is shown. After the
user types the password and the field goes away, Compass offers "Unlock
<name> with biometrics?". The password is sealed with the public half of a
new RSA key pair in the Android Keystore whose private half needs a strong
biometric for each use and is dropped when a fingerprint or face is added
(sealing needs no prompt, as iOS's Keychain write needs no Face ID); the
sealed bytes are in the app's preferences. When the field shows up again,
the biometric prompt opens the password and the script types it in and
submits it as iOS does (Enter, then the button beside the field, then the
form). If the field is still there 10 s later, the password is forgotten
and a dialog says so. "Not now" stops the offers for that extension. The
extensions sheet shows "Unlocks with your fingerprint or face" and a
"Forget password" button; removing (or turning off) the extension forgets
it too. Needs Android 11 (strong-biometric keys).

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
| C7 | Extensions: Web Store installs, 1Password, uBlock, and the desktop's extension list | Web Store installs, the extensions sheet and biometric unlock (`ExtensionUnlock`) done; uBlock Origin Lite and 1Password's popup tested |
| C8 | Claude: chat sheet, page tools, browser tools | Done (page tools; list, open, select and close tabs); not yet tried with a real key |
| C9 | Widgets, Quick Settings tile, shortcuts, share target | Done |
| C10 | Tablets and foldables, keyboard shortcuts, multiple windows | Done |
| C11 | Release pipeline: official build, Play testing tracks, APK on the GitHub release | Working on it: the app bundle (`compass_bundle`), versionCodes from the release version, `compass play upload` to a track, and the listing and App content drafts in `android/play/` (building.md, "Android releases (Google Play)"); on internal testing since 2026-11-18, closed testing in Google's review; `compass release publish-android` signs the release APK with the release key and puts it on the version's GitHub prerelease. `compass play upload` (internal testing by default, `--track production` for the store) and `compass play status` run as a service account since 2026-11-18 (building.md, "Android releases") |

## Parity with iOS

What the iOS app (ios.md) does that Android doesn't yet, in the order
planned. The first group is mostly shell work; splits are the large
piece left.

| # | Scope | Notes |
|---|---|---|
| P1 | Archive | **Done** (above, "The archive"). Closing a Today entry sends it to the space's archive; "Archive" in the sidebar's long-press menu; an archive view from the space menu (sidebar.md's rules, as on the desktop) |
| P2 | Pinned URL | **Done** (above, "Pinned URL"). "Use This Page as Pinned URL" and "Back to Pinned URL" for a pinned tab or favorite; closing one goes back to its pinned URL |
| P3 | On Launch | **Done** (above, "On Launch"). Settings → On Launch: Sidebar (default) or Last Tab; the last shown tab and space are kept as they change, and a launch opens on the last space |
| P4 | Tab picker gesture | **Done** (C4: `CompassBarUi.kt`, "The bar"; the picker opens when the swipe ends rather than following the finger). Swiping up on the bar left of the address pill opens the tab picker, as on iOS (pill: sidebar; right: command bar) |
| P5 | Sidebar drag and drop | **Done**, within the shown space and into the favorites (above, "Drag and drop"); onto other spaces through the menu. Reorder entries and move them between folders, Pinned and Today, and onto other spaces |
| P6 | Splits | **Done** (above, "Splits"). "Split With…" and "Add to Split…" in the long-press menu; a split of two shows both panes (stacked, side by side in landscape) with a handle to resize them, three or four one pane at a time under a pane picker; panes drag to reorder |
| P7 | Profiles | **Done** (above, "Profiles", phases 1–3). Every profile of the chain synced in its own account, as iOS M8 and the desktop do; their spaces in one sidebar, favorites per profile |
| P8 | Compass settings | **Done** (above, "Settings"): search engine, privacy, site settings; a site's own exceptions and the rest on Chrome's pages. Compass's own Settings screens for what's still Chrome's: search engine, privacy, site settings; Chrome's screens stay for the rest |
| P9 | System extras | The share target picks a space and folder (done, above, "Extras"). Today, Quick Actions and Other Devices widgets (done, above); app search for favorites and Pinned entries (done, above, "Extras": launcher shortcuts, the first dozen or so); a space per Do Not Disturb mode or work profile if Android allows it (the Focus filter's role) |
| P10 | Passwords from other apps | **Done** (above, "Passwords in other apps"). Other apps' logins save into Compass (Autofill's save request), not only fill |
| P11 | Link folders | **Done** (above, "Link folders"). A link opened in the background gathers with its page in a Today folder, by the desktop's rules (sidebar.md, "Link folders"; ios.md, "Link folders") |
| P12 | Claude in the sidebar | **Done** (above, "Claude in the sidebar"). Names for link folders (automatic, Suggest a Name and Undo Name), Tidy Today, shorter tab titles, Ask in the command bar (ai-sidebar.md, A2–A5; ios.md) |
| P13 | Links routed to spaces | **Done** (above, "Links routed to spaces"). A web link from another app on a domain with a rule opens as a new Today tab of the rule's space; **Always Open <domain> in ›** in the little window's ⌄ menu; Settings → Sites in Spaces (link-routing.md; ios.md, "Links routed to spaces") |
| P14 | Peek | **Done** (above, "Peek"). Long-press a link to look at it in a card over the page without opening a tab; a pinned tab's links peek; Open keeps the page (peek.md; ios.md, "Peek") |
| P15 | Little windows | **Done** (above, "Little windows"). With Compass as the default browser, a web link from another app opens in Peek's card over what the window shows, not among the tabs (little-window.md; ios.md, "Little windows") |
| P16 | Reader mode | **Done** (above, "Reader mode"). The desktop's Reader on the shared package (`common/reader/`) for a page that reads as an article (reader-mode.md; ios.md, "Reader mode") |
| P17 | Auto picture-in-picture | **Tab switches done** (above, "Auto picture-in-picture"): leaving a tab whose video plays with sound shows it in picture-in-picture, and coming back puts it back; Site Settings turns it off. Not yet: leaving the app, which Android blocks as it's started (a background activity launch) |

Profiles, still the last used profile's: Chrome's own code that reads
`getLastUsedRegularProfile()` (downloads, notifications, some settings),
and saves from other apps (`PasswordSaveActivity`), act on the last used
profile rather than the window's; only the first profile gets web push.

Chrome's look is restyled where it showed most: the link menu and page
info are Compass's, and messages and tab-modal dialogs sit at the bottom
with the bar (above, "The link menu", "The Site sheet", "Messages",
"Dialogs"). Still Chrome's: its settings pages (re-themed), the keyboard's
password and address bar above it, and app-modal dialogs.

Peek and little windows, not yet: the opener for a link
the card opens in a tab (it opens from its URL), the address's
suggestions, and other profiles' spaces in
Open in <space>'s menu. A pinned tab's links that Chrome sends as a
navigation in a new tab (not a new WebContents) still open tabs. Chrome's own top toolbar also
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
