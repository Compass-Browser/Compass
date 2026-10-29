# Profiles in place

Status: spike working on branch `spike/reprofile` (2026-10-29): the
`BrowserView` exchange below ("Direction taken"). The plan up to "Spike
findings" is the first idea, kept for the record. Replaces the window swap of spaces slice 3 (D17,
`sidebar_swap.cc`). Based on Chromium 156.0.8078.48.

## Goal

Switching to a space of another profile happens in the same native window:
same NSWindow/HWND, same macOS Space, same fullscreen, same window to
Mission Control and the OS, same `windowId` to extensions. No second window,
no transparent overlay, no fullscreen animation.

## Choosing the seam

A `Browser` has one profile, and ~120 `BrowserWindowFeatures`, the toolbar
parts the header hosts and ~230 call sites in the window UI alone read
`browser->profile()`. Three ways to get one window with several profiles:

1. **Mixed strip.** One `Browser` holds tabs of several profiles, and the
   window UI follows the active tab's profile. Every `profile()` call site
   has to choose between "the window's" and "the active tab's" profile.
   That's thousands of sites across `//chrome`, and every rebase adds more.
   Rejected (as in §6).
2. **Several `Browser`s in one widget.** Swap which `BrowserView` is the
   `BrowserWidget`'s client view. `views::Widget` can't change its delegate
   or client view after `Init()`, `BrowserWidget`, `BrowserFrameView` and the
   native frame all hold their `BrowserView`, and a `Browser` without a
   widget crashes everywhere. That needs patches in `//ui/views`. Rejected.
3. **Re-profiling the window's `Browser`.** Rebinding (D15) already means a
   window's strip only ever holds the shown space's tabs, so it holds one
   profile's tabs at a time. A cross-profile switch empties the strip of the
   old profile's tabs, gives the `Browser` the new profile, rebuilds what was
   built for the old one, and fills the strip with the new profile's tabs.
   The invariant "every tab in a `Browser` has its profile" holds outside the
   switch itself, so the hundreds of `profile()` readers stay correct. Only
   what *cached* the old profile at construction has to be rebuilt.

**We take 3 (D18).** The work is an inventory of what caches the profile,
and a rebuild path for it.

## The switch, step by step

`strata::ReprofileWindow(browser, new_profile)` (new,
`browser/ui/views/frame/window_reprofiler.{h,cc}`), called by
`SwitchWindowToSpace()` where it used to swap:

1. **Quiesce.** Close the window's own bubbles and browser-modal dialogs (as
   `~BrowserWidget()` does with owned widgets, minus PiP), the find bar, the
   command bar; exit tab fullscreen (window fullscreen stays). Freeze the
   sidebar page mid-slide as `BeginSwap()` does today.
2. **Empty the strip.** The old space's tabs leave as in step 6 of a switch
   (to a window showing that space, else the old profile's parking window),
   favorites' tabs too. `Browser::TabStripEmpty()` must not close the window
   while this runs (patch, below).
3. **Unbind from the old profile:** tear down `BrowserWindowFeatures`
   (`TearDownPreBrowserWindowDestruction()` then destroy), the profile-keyed
   views (toolbar and what the header borrowed from it: location bar,
   extensions container, pinned actions, app menu; bookmark bar; side
   panel; download UI), the `Browser`'s profile-keyed members (command
   controller, location bar model, live tab context, synced window delegate,
   ...: the spike lists them), and the old profile's bookkeeping: its
   `SessionService` sees the window closed (`WindowClosing`/`WindowClosed`
   for this `SessionID`), its `BrowserManagerService` releases ownership, its
   `kBrowserWindow` keep-alive goes.
4. **Bind to the new profile.** `profile_` changes; the new profile's
   `BrowserManagerService` adopts the `Browser`, a `kBrowserWindow` keep-alive
   is taken, its `SessionService` sees `WindowOpened()` (same `SessionID`,
   window bounds and state); then the members, features and views are
   rebuilt in upstream's construction order
   (`BrowserWindowFeatures::Init()`, `InitPostWindowConstruction()`,
   toolbar, `InitPostBrowserViewConstruction()`), and the header reattaches
   its slots as it does after `ToolbarView::Init()`. Theme:
   `BrowserWidget::ThemeChanged()`; the space color seed is set as before.
5. **Fill the strip.** `EnterSpace()` as today: the space's parked tabs come
   in and its last shown entry activates (or a new tab opens).
6. `BrowserList` observers hear `OnBrowserSetLastActive()` again so the
   last used profile follows (profile menu, new windows from the Dock).

Steps 2 to 5 run in one task, so nothing paints a window without tabs. The
WebUIs the window hosts (sidebar, command bar) belong to a profile:

- **Sidebar.** Its page already lists every profile's spaces and slides
  between them, so the page stays. `SidebarPageHandler` and
  `WindowFeatures` rebind to the new profile's `SpaceService` (registry,
  bridge, favorites). The `WebContents` stays in the profile it was created
  in, which never goes away (`kStrataSpaces` keep-alive). If the spike shows
  that pinning a profile-A WebUI into a B window is a problem (devtools,
  extensions, theme), recreate the sidebar `WebView` in B at step 4, behind
  the frozen frame.
- **Command bar.** Closed in step 1; it opens in the window's current
  profile next time (omnibox suggestions are that profile's).

Off-the-record windows never re-profile (they list their own spaces only, as
today), and a window never re-profiles to or from guest or system profiles.

## Window sync

A tab only ever moves between windows showing the same space (§7), and a
space has one profile. With D18 a window's profile is always its space's,
so every move stays inside one profile, as tab dragging requires. Windows
re-profile independently: two windows can show spaces of different profiles
at once.

- Activating an entry live in another window moves its tab here: both
  windows show that space, so the same profile.
- A closing window hands its tabs to a window showing the same space, else
  they close. Unchanged.
- Leaving a space (step 2) sends its tabs to a window showing it, else to its
  profile's parking window. Unchanged.
- Favorites are the profile's: each window shows its profile's, and their
  tabs leave and come in with the rest.
- Placeholders are tabs of the old profile; they close in step 2 as today.
- Parking windows close when the last normal window of every listed profile
  closes, which still fits a profile with no window of its own.
- Upstream's lookups by profile (`FindTabbedBrowser()`, links from other
  apps, the Dock) follow the window because ownership moves
  (`ReleaseBrowser()` and adopt in steps 3 and 4).

Two things need care:

- **Strata's registries are per profile.** The window's `TabStripBridge`
  and its entry in the old `SpaceService` (and `LiveTabRegistry` lookups of
  "which window shows this space") must be dropped in step 3 and made in the
  new one in step 4, or a click in another window could pick a window that
  is now another profile's.
- **The window is empty during the switch.** Steps 2 to 5 run in one task, so
  no other window acts in between; a test covers it anyway.

## Patches

Kept small by putting the sequence in `//strata` and giving it access:

| Patch | Change |
|---|---|
| `chrome/browser/ui/browser.h` | `profile_` not `const`; `friend class strata::WindowReprofiler;` |
| `chrome/browser/ui/browser.cc` | `TabStripEmpty()` returns early while `strata::IsReprofiling(this)` |
| `chrome/browser/ui/browser_manager_service.{h,cc}` | `ReleaseBrowser(BrowserWindowInterface*)` → `unique_ptr`, the counterpart of `AddBrowser()` |
| `chrome/browser/ui/views/frame/browser_view.h` | `friend class strata::WindowReprofiler;` (toolbar, bookmark bar, side panel members) |
| `chrome/browser/ui/browser_window/public/browser_window_features.h` | `friend` for teardown and re-`Init()`, if the spike shows a full rebuild works |
| `chrome/browser/ui/views/location_bar/location_bar_view.h` and others that hold `const raw_ptr<Profile>` | only if the spike finds views we can't simply recreate |

Rebase risk: `WindowReprofiler` repeats upstream's construction order, which
moves every milestone. A browser test that re-profiles a window and then
exercises each rebuilt part (below) is what catches drift.

## Removed

`sidebar_swap.cc`, `BeginSwap()`/`LeaveForSwap()`, the transparent second
window, the "switch at the end of the slide" special case in the page
(cross-profile switches slide and switch like same-profile ones). D17 keeps
the list, assignment and keep-alive parts; its swap is superseded by D18.

## Slices

0. **Spike (throwaway branch, ~1–2 days).** In a browser test, re-profile a
   one-tab window naively (rebuild everything in step 3–4) and run it with
   `enable_dangling_raw_ptr_checks = true` and `DCHECK`s on. Inventory every
   object holding the old `Profile*` or a profile-keyed service after the
   swap (dangling-pointer reports, plus a grep of `raw_ptr<Profile>` and
   `GetForProfile(` in constructors of window features and toolbar views).
   Outcome: the exact rebuild list, and a go/no-go. **No-go** if a full
   `BrowserWindowFeatures` rebuild can't work and the partial list is
   unbounded; then D17's swap stays.
1. **Re-profile core.** The patches above, `WindowReprofiler`, the switch
   wired into `SwitchWindowToSpace()`. Strata's `WindowFeatures` rebinds to
   the new `SpaceService`. Tests below.
2. **Window UI follows.** Header slots reattached, extension toolbar of the
   new profile, theme, app menu items (bookmarks, history, profile),
   downloads button, side panel. Delete the swap code.
3. **Session and startup.** A window quits in the profile it shows, and
   comes back there; the old profile's session no longer has it. Check
   `--restore-last-session` with windows in two profiles. Fixes the
   "upstream may show its profile picker at startup" gap if the last used
   profile is right.
4. **Extensions.** `BrowserExtensionWindowController` is a window feature,
   so its rebuild removes the window from the old profile's extensions
   (`windows.onRemoved`) and adds it to the new one's
   (`windows.onCreated`) with the same id. Confirm `chrome.windows.getAll()`
   in each profile, and that an extension popup or side panel open at the
   switch closes cleanly.

## Tests

Browser tests (`StrataReprofileBrowserTest`, headless):

- The same `BrowserWidget`/native window and `SessionID` after a switch to
  another profile's space and back; `browser->profile()` follows.
- Parked tabs of both profiles survive several round trips, with page state
  (a form value).
- The old profile's `BrowserManagerService` no longer owns the window, the
  new one does; keep-alives move.
- Omnibox suggestions and the command bar come from the new profile's
  history; Cmd+T opens a tab in the new profile.
- Extensions container shows the new profile's extensions; `windows.getAll()`
  in each profile (test extensions in both).
- Session: quit in profile B, relaunch, the window is B's.
- Window fullscreen kept across the switch (by eye in the fast build; the
  test checks `IsFullscreen()`).
- Window sync: two windows on a space of profile A; one switches to a space
  of B. Clicking an A entry live in the switched window from the other one
  doesn't reach it (the tab went to the other window or parking); clicking a
  B entry from a third window on that B space moves the tab across. Closing
  the B window hands its tabs to the third window.
- A bubble open at the switch closes without a crash; a download in progress
  in A keeps going.

Update `SpaceSwapTest` and `AnotherProfilesSpaceSwapsTheWindow` (now: no new
window appears).

## Spike findings (static, 2026-10-29)

The naive plan above doesn't hold:

- `BrowserWindowFeatures::Init()` runs before the window exists, and
  `BrowserView`'s constructor builds almost every view from those features
  (toolbar, contents, side panel, region). ~15 `Init()`-time features cache
  the profile (command controller, cookie controls, signin view controller,
  session helper, theme observer, vertical strip state controller, side
  panel coordinators, ...), and views hold raw pointers into them. Features
  can't be rebuilt while the `BrowserView` lives.
- `BrowserView` can't be rebuilt in the same widget: `BrowserWidget` takes
  it as client view and delegate at `Init()`, and destroying it destroys the
  NSWindow.
- Bookkeeping that reads `browser->GetProfile()` at open and close has to be
  closed for A and opened for B by hand: `ProfileManager::browser_counts_`
  (`erase(find(...))` on a profile never counted: UB), `BrowserManagerService`
  ownership (deleting looks up B's service and silently leaks), the
  `SessionService` window, `TabStripModel::profile_` (no setter), the
  window's `Profile::kProfileKey` property (child widgets read it).
- Cheap to redo: the `InitPostWindowConstruction()` features
  (extension window controller, live tab context, focus controller, ...),
  the bookmark bar (lazy), `BrowserView`'s pref registrar and observations.

So in place means one of:

1. **Rebuild the `BrowserView` inside the same native window**: a patch that
   lets a `BrowserWidget` take a new `BrowserView` (client view and delegate)
   or adopt the old one's native window, then replay `Browser`'s
   constructor. Patches in `BrowserWidget` and likely `//ui/views`.
2. **Targeted rebuild**: keep the `BrowserView`, rebuild the ~15 features,
   the toolbar and everything pointing at them by hand. Many friend
   patches; the list changes every milestone.
3. **Keep the swap (D17)** and work on its gaps instead.

### Direction taken: exchanging `BrowserView`s

The user picked 1. Its prototype keeps everything Chrome builds for a window
as it is and moves it: the other profile's window is built as the swap
builds it today (a real `Browser`, never shown), then the two windows'
`BrowserView`s **exchange native windows**
(`strata::BrowserViewExchange`, `browser/ui/views/frame/`). The shown
NSWindow gets the new profile's view; the old view lands in the hidden
window and empties and closes the way `LeaveForSwap()` already does. Every
`Browser`'s profile stays fixed, so none of the bookkeeping above changes.

What ties a native window to its view, and how the exchange redoes it:

| Tie | Redone by |
|---|---|
| `Widget::widget_delegate_`, `non_client_view_` (client view = the `BrowserView`) | `Widget::ExchangeContentsWith()` (views patch) |
| `BrowserWidget::browser_view_`, `BrowserRootView::browser_view_`, `BrowserNativeWidgetMac::browser_view_` | set by the exchange (`friend`) |
| `BrowserView::browser_widget_` (owns the widget) | swapped |
| Frame view (`BrowserFrameViewMac`, made for one view) | `CreateFrameView()` again |
| `kBrowserViewKey`, `Profile::kProfileKey` window properties | set again |
| Accelerators on the widget's `FocusManager` | unregistered, `LoadAccelerators()` again |
| `widget_observation_`, activation | re-observed; activation replayed |
| Immersive fullscreen overlay widgets (Mac) | reparented |

Not handled yet: the touch bar (`_browser` in the NSWindow subclass), glass
frame subscriptions (feature off), immersive mode controller state,
bubbles anchored at the moment of the exchange.

What changes for the user: the same NSWindow, macOS Space and fullscreen.
To extensions and the session the window is still another window
(new `windowId`), which is accurate: it's another profile's.

## Open questions for the spike

- Does anything outside the window cache `Browser*` + its profile
  (`chrome::FindBrowserWithProfile()` results kept by services, WebUI
  handlers of open tabs, `BrowserWindowInterface` observers in features of
  *other* windows)?
- `Browser` members created only in its constructor that read the profile
  and have no teardown (the spike's list decides whether we patch them into
  re-creatable members).
- macOS app shim and Dock menu: do they track windows per profile?
- Profile-keyed `ui::ColorProvider` / theme caches in the widget.
