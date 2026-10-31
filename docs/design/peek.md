# Peek

Status: first slice done (2026-10-31). Based on Chromium 156.0.8078.48.

## Goal

Arc's Peek: look at a link without opening a tab. Option+click on a link
shows its page in a card over the current page. Close it and nothing is
left behind; keep it and it becomes a Today entry without reloading.

## Using it

- **Option+click** a link (upstream downloads it). Arc puts Peek on
  Shift+click; Compass keeps Shift+click for a new window, as upstream.
- The page shows in a rounded card over the content area, which dims. The
  close (×) and open buttons sit beside the card's top trailing corner.
- **Close:** Esc (after the page, which may use it), Cmd+W, the ×, a click
  on the dimmed area, `window.close()`, switching tabs or spaces, or a click
  on the rest of the window (sidebar, frame). Another app taking focus
  doesn't close it.
- **Keep:** Cmd+O or the open button: the same WebContents becomes the
  active tab, so a new Today entry (the strip's bridge adopts it as any new
  tab), with its history, scroll position and form state.
- Inside the card, links load in the card; links that open a new tab or
  window (`target=_blank`, `window.open()`, Cmd+click) open in the window as
  from the tab, and the new tab showing closes Peek. Option+click in the
  card loads the link in the card.
- The window's shortcuts work from the card (Cmd+T, Cmd+L, ...).

## How

### Getting the click

Blink turns Option+click on a link into the download policy, and
`RenderFrameImpl` sends `DownloadURL`, which carries no modifiers. Upstream
waits the double-click time (500 ms, `ui::kDoubleClickTimeMs`) first, so an
Option+double-click selects text instead; a Blink patch
(`HTMLAnchorElementBase::HandleClick()`) navigates at once, so the card
shows on the click. An Option+double-click on a link peeks (the second click
finds the page already there). A one-condition patch in
`RenderFrameImpl::BeginNavigation()` sends GET http(s) navigations with that
policy as `OpenURL` instead, whose disposition is `SAVE_TO_DISK` (a
disposition renderers may already send). Other downloads (`<a download>`,
blob/data URLs, posts) still go as `DownloadURL`.

`BrowserWebContentsDelegate::OpenURLFromTab()` hands `SAVE_TO_DISK` from a
tab to `compass::PeekOrDownload()` (patch): a normal window with a sidebar
peeks; anything else (app windows, popups, a post) is downloaded from the
browser (`RenderFrameHost::CreateDownloadUrlParameters()`), as upstream
would have.

### The card

`compass::Peek` (`browser/ui/views/peek/`), one per window, owned by the
sidebar view next to the command bar, found with `Peek::From(browser)`.

- The page is a `WebContents` of the window's profile with the tab helpers
  a tab has (permissions, autofill, find, ...): `TabHelpers` lets only a few
  friends attach them, and upstream's removed Link Preview left one,
  `PreviewTab`, which Peek defines. `Peek` is the page's
  `WebContentsDelegate` (navigation, new contents, keys, file choosers,
  camera and microphone).
- The card is a frameless, translucent child widget covering the content
  area (`contents_container()`), following it as it moves or resizes
  (sidebar collapse, window resize). It paints the dim (rounded like the
  content card, 12 DIP), the card (10 DIP, the page clipped with
  `SetNativeViewCornerRadii()`) and the buttons.
- Opening, the card fades in and grows from 92% of its size around its
  center, in 160 ms (ease out). The page is a native view on macOS, out of
  reach of layer opacity and transforms, so the whole widget fades
  (`Widget::SetOpacity()`, the window's alpha) and the card's bounds grow,
  the page laying out again as they do. No animation when rich animations
  are off (tests, Reduce Motion).
- Cmd+W and Cmd+O reach the window as `IDC_CLOSE_TAB` and `IDC_OPEN_FILE`
  (menu shortcuts); `compass::HandleBrowserCommand()` gives them to Peek
  while it shows. The page's delegate also takes them before the page
  (`PreHandleKeyboardEvent()`), and Esc after it.
- Closing hides the widget and lets go of the page after the current event
  (the page or the card may be handling it).

## Not done

- Arc also peeks at links that would open a new tab from a pinned tab.
- Option+double-click no longer selects a link's text.
- Closing has no animation.
- Option+click outside a normal window's tab (extension popups, side
  panels, WebUI bubbles) now does nothing: those delegates ignore
  `SAVE_TO_DISK`.
- No title or URL on the card, no back button, no resizing; JavaScript
  dialogs in the page are suppressed (no tab-modal host).
- Hover cards, the pill and the command bar don't know about the page.

Tests: `PeekBrowserTest.*` (`browser/ui/views/peek/peek_browsertest.cc`).
