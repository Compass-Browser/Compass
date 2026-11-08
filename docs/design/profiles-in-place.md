# Profiles in place

Status: done, as the `BrowserView` exchange below (D18). Replaces the
window swap of spaces slice 3 (D17, `sidebar_swap.cc`). Based on Chromium
156.0.8078.48.

## Goal

Switching to a space of another profile happens in the same native window:
same NSWindow/HWND, same macOS Space, same fullscreen, same window to
Mission Control and the OS. No second window shows, no transparent overlay,
no fullscreen animation.

## Choosing the seam

A `Browser` has one profile, and ~120 `BrowserWindowFeatures`, the toolbar
parts the header hosts and ~230 call sites in the window UI alone read
`browser->profile()`. Ways to get one window with several profiles:

1. **Mixed strip.** One `Browser` holds tabs of several profiles, and the
   window UI follows the active tab's profile. Every `profile()` call site
   has to choose between "the window's" and "the active tab's" profile.
   That's thousands of sites across `//chrome`, and every rebase adds more.
   Rejected (as in §6).
2. **Re-profiling the window's `Browser`.** Rebinding (D15) means a
   window's strip only holds the shown space's tabs, so one profile's tabs
   at a time. A switch would empty the strip, give the `Browser` the new
   profile and rebuild what cached the old one. Rejected after reading the
   code:
   - `BrowserWindowFeatures::Init()` runs before the window exists, and
     `BrowserView`'s constructor builds almost every view from those
     features. ~15 `Init()`-time features cache the profile (command
     controller, cookie controls, signin view controller, session helper,
     theme observer, vertical strip state controller, side panel
     coordinators, ...), and views hold raw pointers into them. Features
     can't be rebuilt while the `BrowserView` lives.
   - `BrowserView` can't be rebuilt in the same widget: `BrowserWidget`
     takes it as client view and delegate at `Init()`, and destroying it
     destroys the NSWindow.
   - Bookkeeping that reads `browser->GetProfile()` at open and close would
     have to be redone by hand: `ProfileManager::browser_counts_`,
     `BrowserManagerService` ownership, the `SessionService` window,
     `TabStripModel::profile_` (no setter), the window's
     `Profile::kProfileKey` property.
3. **Exchanging `BrowserView`s between native windows.** Every `Browser`
   keeps its profile; only the native window moves. Taken (D18).

## The exchange

The other profile's window is built as a real `Browser`, never shown. Then
the two windows' `BrowserView`s **exchange native windows**
(`compass::BrowserViewExchange`, `browser/ui/views/frame/`). The shown
NSWindow gets the new profile's view; the old view lands in the hidden
window, which empties and closes (`BeginSwap()`/`LeaveForSwap()`: its tabs
go to a window showing the old space, else the old profile's parking
window). Every `Browser`'s profile stays fixed, so none of the bookkeeping
above changes.

What ties a native window to its view, and how the exchange redoes it:

| Tie | Redone by |
|---|---|
| `Widget::widget_delegate_`, `non_client_view_` (client view = the `BrowserView`) | `Widget::ExchangeContentsWith()` (views patch) |
| `BrowserWidget::browser_view_`, `BrowserRootView::browser_view_`, `BrowserNativeWidgetMac::browser_view_`; on Windows `BrowserNativeWidgetAura::browser_view_` and `BrowserDesktopWindowTreeHostWin::browser_view_` | set by the exchange (`friend`) |
| Windows: the tree host's `BrowserWindowPropertyManager` (taskbar relaunch command and icon) | made again for the view that comes in (the window icon is the plain app icon for every profile, so it stays, and the taskbar button carries no profile overlay) |
| `BrowserView::browser_widget_` (owns the widget) | swapped |
| Frame view (`BrowserFrameViewMac`, made for one view) | `CreateFrameView()` again |
| `kBrowserViewKey`, `Profile::kProfileKey` window properties | set again |
| Accelerators on the widget's `FocusManager` | unregistered, `LoadAccelerators()` again |
| `widget_observation_`, activation | re-observed; activation replayed |
| Immersive fullscreen overlay widgets (Mac) | reparented |
| Child widgets (bubbles, tab-modal dialogs, find bar, overlays) | each moves with its view: the shown view's go to the hidden window, out of sight; its bubbles (not tab-modal dialogs) close |
| `BrowserView::paint_as_active_subscription_` (the browser's active state) | registered with the new widget; the new browser becomes the last active one if the old one was, as `Show()` does |
| Immersive fullscreen (Mac) | turned off for the leaving view; the view that comes in hears `FullscreenStateChanging()`/`Changed()` and turns it on |
| Touch bar delegate (`_browser` in `BrowserNativeWidgetMac`) | `RebindTouchBar()` (patch) |

A tab-modal dialog (a JavaScript alert too) open in the shown view closes
when the old window closes after the switch: upstream's
`ModalDialogHostObserverViews` closes it with the window it opened in, as it
does when the tab is dragged out of a window that then closes. Its tab
comes back without it. Same-profile space switches keep it (the window
stays).

To extensions and the session the window is another window (new
`windowId`), which is accurate: it's another profile's. Off-the-record
windows never switch profile (they list their own spaces only), and a
window never switches to or from guest or system profiles.

Not handled: glass frame subscriptions in `BrowserNativeWidgetMac` (the
feature is off), and its back/forward command observers (only for PWAs).
The find bar of a view built hidden would keep that window's focus
manager, but a view built hidden never opens one.

Rebase risk: the exchange redoes what upstream ties to a widget at
construction. The browser tests below catch drift.

## One sidebar page

The window keeps its sidebar page through a switch to another profile's
space, as Arc's keeps its sidebar: the page slides to the space as in any
switch, and the new window's handler sends it that window's tree. Nothing
loads, so nothing flashes.

- **Owning the page.** `CompassSidebarView` owns its page's `WebContents`
  itself (the `WebView` has no browser context to make one) and is its
  delegate, as a `WebView` is of its own. A swap's window
  (`IsCreatingSwapWindow()`) starts without a page; `ExchangeTransition`
  moves the old window's to it right after the exchange
  (`TakePageFrom()`), which remakes the hover card and context menu helpers
  for the new view. Without a page to take, the new window loads its own and
  the exchange waits for its tree.
- **The handler.** The page connects once. At `BeginSwap()` the old
  window's `SidebarPageHandler` stops observing and pauses its receiver; the
  page's calls wait. `TakePageFrom()` sets the page's window
  (`webui::SetBrowserWindowInterface()`), and `CompassSidebarUI`, which
  follows it (`RegisterBrowserWindowInterfaceChanged`), takes the pipes from
  the old handler (`TakePipes()`) for one bound to the new window, which
  sends spaces, tree and archive (`RequestTree()`). To the page, the switch
  ends as within a profile: the tree arrives mid-slide.
- **When.** The page asks for the switch as its slide starts; building the
  window, the exchange and the page's move happen in that task, so the rest
  of the window changes at once while the page goes on sliding in its own
  renderer (the compositor lock holds the window's frame, not the page's).
  The new window's tints go on from the old window's color
  (`SidebarTint::ContinueFrom()`).
- **Another profile's page.** The page stays in the profile it was made in,
  which never goes away while listed (`kCompassSpaces` keep-alive). Its
  colors come from the window (`colors.css` is made from the hosting
  widget's color provider). Icons come from `chrome://compass-favicon`
  (`CompassFaviconSource`), which serves `chrome://favicon2` requests from the
  profile the page names (`profile=` a space's `profileId`): the live pane
  asks for the window's profile, a preview for its space's. When the page's
  profile is about to go (unlisted and unloaded), the sidebar loads a page
  of the window's own profile in its place.

The last frame is held for the page: a page still loading waits at most
`kMaxPagePaintWait` (200 ms) and paints in place, and a page that has loaded
only waits for a frame.

The command bar's page, which a window loads as it opens, waits in a swap's
window until the exchange is done (`CompassSidebarView::PreloadCommandBar()`),
so its renderer doesn't launch during the switch. A command bar open at the
switch closes with the other bubbles; it opens in the window's current
profile next time.

## Header and favorites across a switch

Profiles have their own extensions and favorites. When a switch exchanges
the windows, the new header grows or shrinks from the old header's height
to its own (`SidebarHeader::ContinueFrom()`, called next to the tints'
hand-over in `ExchangeTransition::Exchange()`): a profile with more
extensions opens its extra row over the slide's 280 ms instead of pushing
the page down at once, and its extensions and pinned actions fade in. In
the page, a tree whose favorites differ from the ones shown fades the new
tiles in and glides the space below them from where it was
(`glideFavorites_()`).

## Window sync

A tab only ever moves between windows showing the same space (§7), and a
space has one profile. A window's profile is always its space's, so every
move stays inside one profile, as tab dragging requires. Windows switch
independently: two windows can show spaces of different profiles at once.

- Activating an entry live in another window moves its tab here: both
  windows show that space, so the same profile.
- A closing window hands its tabs to a window showing the same space, else
  they close.
- Favorites are the profile's: each window shows its profile's, and their
  tabs leave and come in with the rest.
- Parking windows close when the last normal window of every listed profile
  closes, which still fits a profile with no window of its own.

## Removed

The transparent second window of the first swap (`SwapTransition`). The
other profile's window is still built, hidden, and `BeginSwap()`/
`LeaveForSwap()` still empty the old one. D17 keeps the list, assignment
and keep-alive parts; its swap is superseded by D18.

## Tests

`CompassProfileSwitchBrowserTest` in
`browser/ui/views/frame/profile_switch_browsertest.cc`, and tests in
`compass_sidebar_browsertest.cc` and `auto_pip_browsertest.cc`:

- The same native window, widget delegate and `kProfileKey` after a switch
  and back, and shortcuts moved with the view
  (`AnotherProfilesSpaceKeepsTheNativeWindow`); six switches in a row with an
  infobar (`SwitchingProfilesBackAndForth`).
- Cmd+T opens the new profile's command bar in the shown window, and its tab
  is the new profile's (`CommandBarAndNewTabUseTheNewProfile`).
- A bubble open at the switch closes (`BubbleOpenAtTheSwitchCloses`); a
  tab-modal dialog goes out of sight with its view, and upstream closes it
  when the old window closes (`TabModalDialogLeavesWithItsWindow`). The
  switch can finish, and the old window close, before a test polls again,
  so both follow the widget's visibility changes (`LeavingWidgetWatch`).
- Page state across two round trips (`FormValueSurvivesSwitches`).
- Fullscreen kept, and left as usual afterwards (`FullscreenIsKept`).
- Pages take colors from the shown widget
  (`PageColorsComeFromTheShownWindow`).
- `windows.getAll()` lists the window in the new profile only
  (`ExtensionsSeeTheNewProfilesWindow`).
- The last used profile follows (`LastUsedProfileFollows`).
- An extension shortcut of the new profile is registered in the window and
  leaves with its view (`ExtensionShortcutsMoveWithTheView`).
- Two windows on a space, one switches: its tabs go to the other
  (`TabsGoToAnotherWindowOnTheSpace`).
- Four switches back and forth keep one sidebar page, which shows each new
  window's space and stays in its own profile (`SwitchesKeepTheSidebarPage`).
- Another profile's icons come from that profile
  (`IconsComeFromTheirProfile`).
- Picture-in-picture's "back to tab" switches the window back to a parked
  tab's profile (`BackToTabSwitchesProfiles`).

Not covered by a test: session restore of a window that switched profiles
(`--restore-last-session` by hand), and downloads (the profile's, not the
window's: a test with a held response timed out before the switch, in its
own setup).

## Notes

The log line `Requested load of chrome://newtab/ for incorrect profile type`
seen on switches is upstream's: `WebContentsImpl::UpdateWebContentsVisibility()`
looks up the new tab page's WebUI config by its committed URL
(chrome://newtab/) whenever its visibility changes. No tab opens in the
wrong profile (`CommandBarAndNewTabUseTheNewProfile`).
