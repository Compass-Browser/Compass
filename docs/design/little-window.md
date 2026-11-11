# Little windows

Status: first slice done. Based on Chromium 156.0.8078.48.

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
- The **⌄ button** beside it (shown when the profile has more than one
  space) lists the profile's spaces, the default checked. Picking one moves
  the page into the last active window showing it, else switches the last
  active window to it.
- The address can be edited, as in a normal window's location bar
  (suggestions, Enter loads it in the little window; Cmd+L focuses it).
- **Cmd+Option+N** opens an empty little window (the new tab page) centered
  on the current window's display, with the address focused. It
  takes upstream's New Split Tab shortcut; Compass splits by dragging a tab
  onto another or from the menu.
- The page isn't in the sidebar while it's in a little window; it becomes
  an entry when it moves into a space.
- A sidebar entry dragged out of the windows opens in a little window where
  it's dropped ([sidebar.md](sidebar.md), "Dragging out of the windows").
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
  - The location bar isn't in popup mode (`toolbar_view.cc` patch:
    `IsCreatingLittleWindow()` while the toolbar is made), so its omnibox
    isn't read-only and has suggestions. `OpenNewLittleWindow()` focuses it
    (`SetFocusToLocationBar()`).
  - Upstream's popup layout insets the page by
    `GetPopupContentFrameInsets()` and drops the separator for it;
    `MultiContentsView::SetBackgroundRadii()` rounds it.
  - `BrowserView::NonClientHitTest()` asks `IsOnLittleWindowCaption()`
    (patch): the frame and the bar's empty space drag the window.
- Moving: `OpenLittleWindowInSpace(browser, space)` detaches the tab and
  appends it to the target window's strip (`FindLastActiveWindowShowing()`,
  else `FindLastActiveWindow()` switched with `SwitchWindowToSpace()`),
  whose bridge adopts it as any new tab. Cmd+O reaches it as
  `IDC_OPEN_FILE` through `compass::HandleBrowserCommand()`.
- Out of the sidebar: the window is created under
  `CreatingWindowOutsideSidebar()`, so its `WindowFeatures` gets a
  `TabStripBridge` that mirrors nothing (`mirrors_tabs = false`). Before,
  every popup's bridge put its tab in the active space's Today, and a page
  moved into another space's window kept that entry. Other popups still do.
- The space menu: `LittleSpaceMenuButton`, a `views::MenuRunner` over a
  `ui::SimpleMenuModel` of the profile's `SpaceList`; same-profile spaces
  only (a WebContents can't change profile). Each item shows its space's
  icon (`GetSpaceIconImage()`, `views/sidebar/space_icon_image.h`): a
  symbol as a vector icon made at run time from `space_icons.ts`'s path
  (embedded with `build/embed_text.py`, parsed by `SkParsePath`; macOS
  menus show it as a template image), an emoji drawn into an image.
- Cmd+Option+N: `kNewLittleWindowCommand` (`compass_command_ids.h`, 39950,
  outside upstream's IDs) in the shortcuts outside the main menu
  (`AdjustShortcutsNotInMenu()`); `IDC_NEW_SPLIT_TAB` loses its key
  equivalent. With browser actions on, upstream's command updater only knows
  commands that have an action, so `BrowserCommandController` hands
  Compass's own commands to `HandleBrowserCommand()` before its
  supported/enabled check (patch). There's no File menu item (Compass has
  no string resources of its own).

## From any app

The shortcut (Cmd+Option+N, Shift+Alt+N on Windows, or the user's from
the shortcuts page) works from any app, with or without a Compass window:
`LittleWindowHotKey` (`little_window_hot_key.{h,cc}`, macOS and Windows)
registers it as a system hot key through upstream's
`GlobalAcceleratorListener` (a Carbon hot key on macOS, `RegisterHotKey()`
on Windows), as picture-in-picture's click-through does, from the first
Compass window on (`CompassSidebarView`) for as long as Compass runs.

- The hot key takes the keys while Compass is in front too. With a Compass
  window active, it opens the little window from it, as the window's
  shortcut does (`OpenNewLittleWindow()`).
- From another app, the little window is of the last active window's
  profile (else the last used one) and opens over that app's window, as a
  link from it would (`OpenNewLittleWindowOverOtherApp()`, macOS's
  `FindOpenerWindow()`), on the pointer's display without one.
- A changed shortcut moves the hot key; a removed one drops it. While the
  shortcuts page records keys it lets them go, or it would take them.
- If another app already holds the keys, registering fails and only
  Compass's own windows have the shortcut.
- Linux: not yet (its shortcuts are upstream's).
- Test: `LittleWindowBrowserTest.HotKeyFromAnyApp` (macOS). Windows: not
  yet tried on a device.

## Not done

- Other profiles' spaces in the picker (the page would have to reload in
  that profile).
- A File menu item.
- A new little window starts on the new tab page (with the address
  focused) instead of Arc's command bar.
- Settings: off, and per-domain routing to spaces (Arc's Air Traffic
  Control).
- Windows: written, not yet built or tried. Links from other
  apps arrive on the command line, through the process singleton when
  Compass is running. `StartupBrowserCreatorImpl::DetermineURLsAndLaunch()`
  (patch) opens them in little windows when `CanOpenInLittleWindows()`
  (not off the record, not with `--new-window` or `--same-tab`): with
  Compass running, nothing else opens; at startup, the usual windows open
  first without the links (`StartupTabProviderWithoutLinks`), then the
  little windows. **Shift+Alt+N** opens an empty one (`accelerator_table.cc`
  patch), taking upstream's New Split Tab shortcut as on macOS; Ctrl+Alt+N
  can't be used, Windows reserves Ctrl+Alt (AltGr). Still macOS-only: the
  frame (no native title bar; the window buttons in the bar), placing it
  over the opener's window, and keeping Compass's other windows behind it.
- Linux: the same hook would do, but it's Windows-only for now.

Tests: `LittleWindowBrowserTest.*` (`TheAddressCanBeEdited`,
`DraggedOutTabMovesThere` among them)
(`browser/ui/views/little_window/little_window_browsertest.mm`), including
the real `application:openURLs:` path.
