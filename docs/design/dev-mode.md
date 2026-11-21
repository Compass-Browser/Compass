# Development mode

Status: desktop (Mac, Windows, Linux) written, not yet built; iOS lite
written ("iOS" below). Based on Chromium 157.0.8103.51.

## Goal

A web developer working on a site turns on development mode for its host,
and on that site:

- **Reload always gets a fresh copy.** The reload button, Cmd+R, the
  command bar's Reload and the tab menu's Reload skip the cache, as
  Cmd+Shift+R does.
- **A bar above the page** shows the page's full URL and has buttons for
  DevTools' Console, its Network panel, picking an element to inspect, and
  copying the URL.
- **The Network panel lists the requests made before it opened.** Opening
  DevTools after something went wrong doesn't mean reloading to see what
  the page asked for.

Development mode is per host, per profile, and stays on the device (it
doesn't sync: the sites a developer works on belong to the machine).

## The sites

`compass.dev_mode.hosts` (`browser/prefs/compass_prefs.h`): a list of host
patterns, matched with Boosts' `BoostMatch` (boosts.md), so:

- `localhost` or `example.com`: that host on any port and scheme (http,
  https), `www.` included;
- `*.example.com`: the domain and its subdomains.

`compass::dev_mode::NormalizeHost()` (`browser/dev_mode/dev_mode.{h,cc}`)
keeps what the user types leniently: `http://localhost:3000/app` is
`localhost`. Paths aren't kept: development mode is about a host.

Turning it **on** for a page adds its host. Turning it **off** for a page
removes every pattern that matches it, so it's always off afterwards even if
`*.example.com` put it on.

### Where it's turned on and off

- **The site information popup** (the sidebar header's site button): a
  "Development mode" row with a switch, for web pages.
  `compass::AddDevModePageInfoRow()` (`browser/ui/views/dev_mode/`), which
  upstream's `PageInfoMainView` calls.
- **The command bar**: "Development Mode for This Site" or "Leave
  Development Mode" (command-bar.md).
- **Cmd+Shift+D** (Ctrl+Shift+D on Windows and Linux): toggles it for the
  active tab's site (`kToggleDevModeCommand`). Upstream's key was Bookmark
  All Tabs, which Compass removes. Customizable in the Developer section
  (shortcuts.md).
- **Settings → Development Mode** (`compass://settings/devMode`,
  `resources/sidebar/settings/dev_mode_page.ts`): the list, with add and
  remove.

## Reload

Upstream's `chrome::Reload()` reloads the selected tabs through
`ReloadInternal()`. The patch asks `compass::dev_mode::ShouldBypassCache()`
for each tab and reloads it `BYPASSING_CACHE` when its page is in
development mode, including when DevTools is docked and does the reload. The
sidebar's tab menu, which reloads directly, does the same.

## The bar

`DevModeBar` (`browser/ui/views/dev_mode/dev_mode_bar.{h,cc}`), a native
view 32 px tall at the top of a pane:

- the URL in a read-only text field, so it can be selected and scrolled
  through however long it is;
- **Console**: DevTools on the Console panel;
- **Network**: DevTools on the Network panel;
- **Inspect**: DevTools in element-picking mode; the element clicked next
  shows in the Elements panel;
- **Copy**: the URL exactly as shown (unlike Copy URL elsewhere, which
  takes out tracking parameters), with a check for a moment.

Each button acts on its pane's page, so each side of a split has its own
bar. A DevTools window that's already open only comes to the front when
asked again (upstream's `DevToolsWindow::Show()` switches no panel), so the
bar also tells its frontend to switch (`DevToolsAPI.showPanel`) through
`compass::dev_mode::ShowDevToolsPanel()`.

`DevModeController` (one per window, owned by the sidebar view as
`ReaderMode` is) adds a bar to each pane whose page is in development mode
and removes it otherwise. It re-checks when the active tab changes, a pane
shows another tab, a pane's page commits a new page, and the list of sites
changes.

The bar takes the top of the pane: upstream's
`ContentsContainerView::CalculateProposedLayout()` takes
`compass::GetContentsTopBarHeight()` off the contents' bounds, so the page
and docked DevTools sit below it. The bar paints the card's top corners
(window-frame.md).

## Requests before DevTools

DevTools' Network panel only shows what happens after its own
`Network.enable`; Chromium keeps nothing for it beforehand. Compass records
instead, and hands the frontend what it recorded.

### Recording

`DevModeTabHelper` (`browser/dev_mode/network_log.{h,cc}`), attached to every
tab, owns a `NetworkLog` while the tab's page is on a site in development
mode:

- an internal DevTools session (`compass::claude::DevToolsSession`, which
  shows no "being debugged" bar) on the page's main frame, with
  `Network.enable`, keeping response bodies as Claude's network tool does
  (50 MB in all, 10 MB per response, 64 KB of request body);
- a `NetworkLogBuffer` of the events the Network panel builds its rows
  from (`requestWillBeSent`, `responseReceived`, their `ExtraInfo`,
  `loadingFinished`, `loadingFailed`, WebSocket, EventSource and
  WebTransport events; not `dataReceived`), grouped by request: 1000
  requests at most, the oldest dropped first, and 200 events per request at
  most (a WebSocket's later frames).

The log empties when the tab starts a new page, before that page's own
request, as the panel does without "Preserve log". It starts when a page on
such a site starts loading, or when the site is added while the page is
showing (then from that moment). It stops when the page leaves the site or
the site leaves the list.

### Replay

`DevToolsFrontendHook` (`browser/dev_mode/devtools_frontend_hook.{h,cc}`),
one per DevTools window, sits between upstream's `DevToolsUIBindings` and
the page (a patch calls it with each protocol message, both ways):

1. It learns the frontend's session with the page's main frame from
   `Target.attachedToTarget` (its target ID is the page's).
2. When the frontend sends `Network.enable` on that session, the hook
   waits for the reply and, right after handing it on, dispatches the
   recorded events to the frontend with that session's ID, in order. The
   page's own events for the session come after these, so a request still
   loading carries on from the replayed ones.
3. The frontend's own session never saw the replayed requests, so
   `Network.getResponseBody` and `Network.getRequestPostData` for one of
   them are answered by the log's session, which did.

Everything else passes untouched. The hook only parses a message after a
cheap substring check, and never one over 64 KB.

Limits:

- Requests from out-of-process iframes and workers aren't recorded before
  DevTools opens (the log's session is the main frame's).
- Replaying a request from the panel (Replay XHR) or searching in bodies
  works only for what the frontend's session saw itself.

## iOS

WKWebView has no DevTools of its own (Safari's Web Inspector on a Mac
attaches to Compass's pages, as `isInspectable` allows), so iOS has a lite
development mode:

- the sites, as on the desktop, kept on the device (`DevModeHosts` in
  `ios/App/Model/`);
- reload (the button and Cmd+R) uses `reloadFromOrigin()` on those sites;
- a bar above the page with the full URL and a Copy button;
- turned on and off from the page menu, Settings → Development Mode, and
  Cmd+Shift+D on a hardware keyboard.

## Tests

- `compass_unittests`: `DevModeTest` (patterns, adding, turning on and off
  for a page), `NetworkLogBufferTest`, `DevToolsFrontendHookTest` (replay
  only after the page session's `Network.enable`, bodies from the log, a new
  session starting over), `ShortcutsTableTest` (Ctrl+Shift+D).
- `compass_browsertests`: `SettingsSectionsBrowserTest.DevModeSection`.
