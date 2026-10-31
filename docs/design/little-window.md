# Little windows

Status: first slice done (2026-10-31). Based on Chromium 156.0.8078.48.

## Goal

Arc's Little Arc: a link opened from another app (Mail, Slack, a terminal)
doesn't land among your tabs. It opens in a small window of its own; read it
and close it, or keep it and move it into a space.

## Using it

- With Compass as the default browser, a link from another app opens in a
  little window, 900×940 (at most 90% of the screen), centered on the
  display of the window the link came from (the pointer's display if none).
- It comes over that app's window and nothing else: Compass's other windows
  stay where they were in the stacking order. With Compass under Terminal,
  a link from Terminal gives little window → Terminal → Compass.
- It looks like the main window: no native title bar; the frame is the
  space's color (tint and theme seed) from edge to edge, with the page as a
  rounded card (8 DIP inset, 12 DIP corners) and no separator over it. One
  40 DIP bar at the top holds the traffic lights, back (no forward, as in
  Arc), the address as a pill, and **Open in <space>** as a pill, naming
  the last active window's space; the frame's color and the name follow as
  you switch windows. The frame and the bar's empty space drag the window.
- **Open in <space>** or **Cmd+O** moves the page, as it is (no reload),
  into that window as the active tab: a new Today entry. The little window
  closes. With no window open, a new one is made.
- Links that open a new tab go to the last active window, as from any
  popup. Extensions work in the page (content scripts, permissions,
  downloads); the window has no extension buttons.
- Little windows aren't restored after quitting.

Anything but web pages and files (other schemes, `.webloc` shortcuts, a
mix) opens as upstream does.

## How

- macOS hands URLs from other apps to `-[AppController
  application:openURLs:]`, which ends in
  `OpenStartupTabsInBrowserWithProfile()` once the profile is loaded (also
  for the URLs a cold launch was started with). A patch there asks
  `compass::OpenInLittleWindows()` first
  (`browser/ui/views/little_window/`).
- The opener: before any window opens, the frontmost on-screen window of
  another app (`CGWindowListCopyWindowInfo`, `little_window_mac.mm`). Its
  display places the window. After showing it, Compass's windows above the
  opener's are put back under it in their order (`-orderWindow:NSWindowBelow
  relativeTo:`), as macOS raises every window of an app it activates; again
  300 ms later and on `NSApplicationDidBecomeActiveNotification` for 2 s, in
  case activation comes later.
- A little window is upstream's popup `Browser` (`TYPE_POPUP`: location-only
  toolbar, page info, find, permission prompts, JS dialogs, downloads), made
  with `omit_from_session_restore` and without triggering session restore.
  - While it's created (`IsCreatingLittleWindow()`),
    `BrowserNativeWidgetMac::PopulateCreateWindowParams()` gives it a normal
    window's NSWindow (full-size content view, transparent title bar) with
    the title hidden, and `GetWindowFrameTitlebarHeight()` returns the bar's
    height so macOS centers the traffic lights on it (patch).
  - `LittleWindowFrame`, the browser view's first child, paints the frame
    color and the space's tint (`GetSpaceFrameTint()`, as `SidebarTint`)
    and seeds the window's theme; not off the record or with an extension
    theme.
  - The toolbar (a horizontal `FlexLayout` in location mode) loses its
    background and gets a fixed 40 DIP height, a spacer for the traffic
    lights, `LittleBackButton` (an upstream `BackForwardButton` following
    `IDC_BACK`) and `LittleWindowButton` ("Open in", styled as the sidebar
    header's pill), which also marks the window (`IsLittleWindow()`). The
    location bar draws its rounded background instead of the popup's
    rectangle (`location_bar_view.cc` patch).
  - Upstream's popup layout insets the page by
    `GetPopupContentFrameInsets()` and drops the separator for it;
    `MultiContentsView::SetBackgroundRadii()` rounds it.
  - `BrowserView::NonClientHitTest()` asks `IsOnLittleWindowCaption()`
    (patch): the frame and the bar's empty space drag the window.
- Moving: `OpenLittleWindowInSpace()` detaches the tab and appends it to
  `FindLastActiveWindow()`'s strip, whose bridge adopts it as any new tab.
  Cmd+O reaches it as `IDC_OPEN_FILE` through
  `compass::HandleBrowserCommand()`.

## Not done

- A space picker (Arc lets you pick the space); Arc's Cmd+Option+N for a
  new empty little window.
- Settings: off, and per-domain routing to spaces (Arc's Air Traffic
  Control).
- Other platforms: links from other apps come through the process
  singleton and `StartupBrowserCreator` there, not this hook.

Checked by screenshot (2026-10-31): a link opened from iTerm with Compass
behind it.

Tests: `LittleWindowBrowserTest.*` (6)
(`browser/ui/views/little_window/little_window_browsertest.mm`), including
the real `application:openURLs:` path.
