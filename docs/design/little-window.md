# Little windows

Status: first slice done (2026-10-31). Based on Chromium 156.0.8078.48.

## Goal

Arc's Little Arc: a link opened from another app (Mail, Slack, a terminal)
doesn't land among your tabs. It opens in a small window of its own; read it
and close it, or keep it and move it into a space.

## Using it

- With Compass as the default browser, a link from another app opens in a
  little window: about 760×820, centered on the screen with the pointer,
  each further one offset a little. Several links, several windows.
- The window is the page with a slim bar: back (no forward, as in Arc), the
  site's address and page info, and **Open in <space>**, naming the space
  of the last active window (it follows as you switch windows).
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
- A little window is upstream's popup `Browser` (`TYPE_POPUP`, so it has a
  location-only toolbar, page info, find, permission prompts, JS dialogs,
  downloads), made with `omit_from_session_restore` and without triggering
  session restore. Compass adds a back button (`LittleBackButton`, an
  upstream `BackForwardButton` that follows `IDC_BACK`) at the start of its
  toolbar and "Open in <space>" (`LittleWindowButton`) at the end; the
  toolbar is a horizontal `FlexLayout` in location mode, so no patch.
  `LittleWindowButton` also marks the window (`IsLittleWindow()`).
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
- The space's color on the window.

Tests: `LittleWindowBrowserTest.*`
(`browser/ui/views/little_window/little_window_browsertest.mm`), including
the real `application:openURLs:` path.
