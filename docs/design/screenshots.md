# Screenshots

Status: desktop (Mac, Windows, Linux). Based on Chromium 157.0.8103.51.

## Goal

Arc's Capture: take a picture of part of a page, or all of it, and copy it
or keep it, without an extension.

## Capture Screenshot

"Capture Screenshot…" is in the command bar (`kCaptureScreenshot`) and the
page's context menu (`kCaptureScreenshotContextCommand`), for web pages
(http, https) and files, off the record too. It puts Capture to Easel's
overlay on the page (easels.md, "Captures") in its screenshot mode
(`__compassEasel.start([], {screenshot: true})`):

- **Drag** over a region, then pick **Copy** (focused, so Enter takes it)
  or **Save to Downloads**;
- **Full Page** (a button in the overlay's hint) takes the whole page,
  then the same choice;
- **Esc** cancels.

`CaptureSession` (`browser/ui/easels/easel_capture.cc`) runs both modes:
it polls the page script, and on a screenshot pick calls
`browser/ui/easels/page_screenshot.{h,cc}`:

- a region is copied from the page's surface as Capture to Easel's is
  (`CopyPageRegion()`, device pixels);
- the full page comes from an internal DevTools session
  (`compass::claude::DevToolsSession`, which shows no "being debugged"
  bar): `Page.getLayoutMetrics` for the content size, then
  `Page.captureScreenshot` with `captureBeyondViewport` as a PNG, at most
  16384 CSS pixels tall (`kMaxFullPageHeight`; the toast says when it was
  cut);
- **Copy** writes the image to the clipboard
  (`ui::ScopedClipboardWriter::WriteImage()`);
- **Save** writes a PNG, off the UI thread, to the profile's download
  folder as "Screenshot 2026-11-23 at 14.05.09.png" (local time; a number
  is added if the name is taken).

A toast on the page says what happened ("Copied to the clipboard", "Saved
to Downloads as …", or why not).

## Not yet

- A keyboard shortcut (Arc's is Cmd+Shift+2); the shortcuts table has none
  yet.
- Screenshots in a Library: saved screenshots are plain files in the
  download folder, and the downloads bubble doesn't list them.
- Picking a page element instead of dragging.
- Android and iOS.

## Tests

`compass_browsertests`: `EaselCaptureBrowserTest.ScreenshotCopiesARegion`
(drag, Enter, the clipboard's image at the region's size and color),
`ScreenshotSavesTheFullPage` (a 3000-pixel page in full, in the download
folder), `ScreenshotFileNameTest`; `CommandBarPageHandlerTest` lists the
command with its own symbol.
