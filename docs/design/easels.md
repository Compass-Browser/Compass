# Easels

Canvas boards in the sidebar: text, shapes, freehand drawing, images, link
cards and captures of page regions, placed freely and synced to every device.
Arc users bring theirs over with the Arc import ("Arc's Easels", below).

Status: desktop (macOS, Windows, Linux), Android and iOS. Captures are
still images with a manual Refresh; live captures that refresh on their own
are a later step ("Later").

## What a board is

A board (`CompassEasel`) has a title and a background (`palette`: `paper`,
`grid`, `dots`, `dark`). On it are objects (`CompassEaselObject`), each with a
box (x, y, width, height in board points from the top left) and a `z` order,
and exactly one kind of content:

| Content | Fields | Shows |
|---|---|---|
| `text` | text, color, font (`sans`/`serif`/`mono`/`hand`), style (`title`/`heading`/`body`/`caption`) | Wrapped text in the box |
| `shape` | kind (1 rectangle, 2 circle, 3 arrow, 4 freehand), color, points, stroke width, filled | Vector drawing |
| `image` | asset, pixel size | The image, fitted to the box |
| `link` | url, title, description, image asset | A card: image, title, host; opens the URL |
| `capture` | url, title, image asset, favicon asset, region, scroll, viewport, capture time | The captured region; opens the URL; Refresh takes it again |

Images (`CompassEaselAsset`) are stored once by SHA-256 and pointed at by
objects. At most 2048 px a side and 512 KiB (bigger ones are scaled down and
re-encoded as JPEG at quality 85, or PNG when they have transparency).

Limits: 200 boards, 1,000 objects a board, 20,000 characters a text, 4,000
points a freehand stroke (longer strokes are simplified), 64 MiB of images a
profile.

The full wire format, field by field, is in
`components/sync/protocol/compass_spaces_specifics.proto` (the `CompassEasel*`
messages). Every platform keeps boards in that form, so nothing is lost
between versions: a field or content kind a client doesn't know is kept and
passed on. An object whose content it doesn't know shows as a grey box that
can be moved and deleted but not edited.

## In the sidebar

A board is a tab whose URL is `compass://easel/<uuid>` (the board's
`CompassEasel.uuid`), pinned or in Today like any tab, with the easel icon and
the board's title. So it moves, archives, splits and syncs as a tab already
does, and an older client shows it as a tab it can't open rather than losing
it. Closing or deleting the tab doesn't delete the board: boards are listed,
and deleted, in the Easels page (`compass://easel/`), and a board that no tab
points at is still there.

New Easel: the Pinned header's easel button (desktop), the space header's
⋯ menu (Android), the sidebar's + menu (iOS), the command bar ("New Easel")
and the Easels page. It makes a board and pins a tab for it in the current
space. Easel tabs get an easel icon (`resources/sidebar/favicon.ts`,
Android `SiteIcon()`, iOS `SiteIcon`). `EaselTitleFollower`
(`browser/ui/easels/easel_titles.h`, run by `SpaceService` and
`SpacesAndroid`) keeps the tabs' titles in step with the board's, local or
synced renames alike; a name the user gave the tab wins.

## Editor

`compass://easel/<uuid>` is the editor (`chrome://easel/<uuid>`, a
WebUI page; on iOS a native view in place of the web view). The same page is
the editor on the desktop and on Android.

- Tools: select (move, resize, multi-select with a drag or Shift), text,
  rectangle, circle, arrow, pen (freehand), image (file picker), and paste:
  an image pastes an image, a URL pastes a link card, other text a text
  object. Dropping files, images or links from a page works the same way.
- Arrange: bring to front / send to back, duplicate, delete, undo and redo
  (the page's own history, not synced).
- Pan with a drag on empty space (or two fingers), zoom with pinch or
  Ctrl/⌘ + wheel; "Fit" shows everything.
- Color: a small palette for text and shapes, stored as CSS colors so every
  platform draws the same: ink (no color set: the board's ink, dark on
  light boards and light on `dark`), red `#d1242f`, orange `#e16f24`,
  yellow `#d4a72c`, green `#1a7f37`, blue `#0969da`, purple `#8250df`,
  grey `#6e7781`.
- Text sizes in board points: title 32, heading 22 (semibold), body 16,
  caption 12; `hand` is a handwriting face (Noteworthy on iOS, a cursive
  stack on the web).
- New boards are "Untitled Easel".
- Title: edited in place at the top; the tab follows it.
- Opening a link or capture: click opens it in a new tab of the space.

Edits save at once (each object an entity; the editor batches a drag into
one update when the pointer is released).

Desktop and Android (`browser/ui/webui/easel/`, `resources/easel/`, its own
resource pack so Android ships it too):

- Mouse: a drag on empty space selects; Space or the middle button and a
  drag, or scrolling, pans; Ctrl/⌘ and the wheel zooms; a double-click on
  empty space adds text. Touch: one finger on empty space pans, two pinch.
  Right-click or long-press: Open, Refresh, Edit Text, arrange, duplicate,
  delete.
- Keys: V, T, R, O, A, P, I pick a tool; ] and [ bring to front and send to
  back; ⌘D duplicates, ⌘A selects all; Shift+1 fits, Shift+0 is 100 %;
  arrows nudge.
- New text is saved once it has text; emptying a text deletes it.
- Each object in the page carries its full stored message, so what the page
  doesn't understand (captures' fields, unknown kinds, a newer version's
  fields) is saved back unchanged.
- Images are served at `chrome://easel/asset/<sha256>`.
- Link cards: the page is fetched without cookies, its first 1 MB, for its
  Open Graph, then Twitter, then `<title>`/description tags and preview
  image; a failed fetch still gives a card with the URL.
- The view (pan and zoom) is remembered per board on each device.
- iOS (`ios/App/Easels/`): a drag on empty space pans, a pinch zooms, and
  multi-select is "Select Area" in the ⋯ menu; Undo is the editor's own
  history.

## Captures

"Capture to Easel" (command bar and the page's context menu on the desktop,
the page menu on Android and iOS) puts a selection overlay on the current
page: drag a region, then pick the board (the last one used this session is
first; "New Easel" makes one, only once the image is stored, and pins its
tab). On the desktop the overlay is `browser/ui/easels/easel_page.js` in an
isolated world, a top-layer popover in a closed shadow root; the browser asks
it for the pick every 120 ms, and it hides itself before the copy. Android
captures the whole visible page instead (dragging fights the page's
scrolling) and picks the board in a dialog. The region is captured from the page's surface
(`RenderWidgetHostView::CopyFromSurface` with the region scaled to device
pixels; on iOS `WKWebView.takeSnapshot(with:)` with the region), encoded off
the main thread, and added to the board with the URL, title, icon, region,
scroll offset and viewport size. On the board it's placed at its CSS size,
to the right of what's there (40 points apart; at 40, 40 on an empty board):
the editor's view isn't known while capturing. A short toast says where it
went.

Refresh (a capture's menu) loads the URL in a hidden page of the saved
viewport size (desktop: a standalone page of the profile, off-screen, told
it's visible while captured; iOS: an offscreen web view), waits for the load
(at most 20 s) plus 1.5 s, scrolls to the saved offset, and captures the
same region again (shifted by any difference in scroll); the old image is
replaced. At most 45 s. Not on Android yet: a page renders there only in a
view attached to a window. Pages that
need the user's session work, because the hidden page uses the profile's
cookies.

### Live captures

On the desktop, captures keep themselves current while their board is
open: when the editor loads, every 5 minutes after (`LIVE_CHECK_MS`) and
whenever the page shows again, it refreshes each capture taken more than an
hour ago (`LIVE_STALE_MS`, by `capture_time`), one at a time and without
toasts, through the same `RefreshCapture()` as the menu's Refresh. A
refresh that fails waits for the next check. Boards the profile only views
aren't refreshed (their captures are the owner's), and neither are hidden
pages. Because `capture_time` syncs, another device with the board open
sees the new image and doesn't take it again. Android and iOS boards stay
static (refreshed by hand on iOS).

## Sync

Each board, object and image is its own entity of `COMPASS_SPACES`
(docs/design/sync.md, "Easels"), so two devices editing different objects of
one board both keep their edits; for the same entity, the later
`update_time` wins. Objects whose board isn't there yet are kept and shown
once it comes. An image no object points at is deleted after 30 days by
whichever device notices.

## Storage

- Desktop and Android: `EaselStore` (`browser/easels/easel_store.h`), a
  profile keyed service. Boards and objects are in
  `<profile>/Compass Easels/easels.pb` (an `EaselStoreFile` proto of the
  sync messages), images in `<profile>/Compass Easels/assets/<sha256>`.
  Not in Incognito or Guest.
- iOS: `CompassEasels` (`ios/CompassKit/Sources/CompassEasels`), the same
  messages in the app's container.

## Arc's Easels

The Arc import (docs/design/arc-import.md) brings Arc's easels over, on
macOS (Arc for Windows has none). Arc keeps only a pointer in
`StorableSidebar.json` (an item whose `data.easel` has `easelID` and
`title`); the boards themselves are in Arc's Firestore cache, a LevelDB at
`~/Library/Application Support/firestore/Arc/bcny-arc-server/main/`, as
`google.firestore.v1.Document`s:

- `easels/<id>`: `title`, `palette`, `lastModified`.
- `easels/<id>/objects/<id>`: `position{rect{x,y,width,height}, zIndex}`
  and one of
  - `content.text{string, style{fillStyle, fontFamily, paragraphStyle}}`
  - `content.shape{color, type: isRectangle | isCircle | arrow{start,end} |
    freehand[{x,y}]}`
  - `content.image{content.url, size}`
  - `content.link{source, title, description, image}`
  - `content.webCard{url, text, captureType.partialPage,
    captureLayoutInfo{frameRelativeToViewport, webContentOffset,
    webContentSize}, image, favicon, dateCaptured}`

Images are Firebase Storage download URLs with their token, fetched over
HTTPS without signing in to Arc. The importer copies the LevelDB (Arc is
quit first), reads every `easels` document, maps each object to its Compass
kind (a `webCard` becomes a capture), downloads and stores the images, and
puts the board's tab where Arc's sidebar item was. Ids are derived from Arc's
(`ArcDerivedId`), so importing again or on a second Mac doesn't duplicate
boards. A board Arc never downloaded to this Mac comes over empty, and the
result says to open it in Arc and import again: "Import Easels Again" on the
import page quits Arc and fills the boards that came over empty.

What Arc's numbers mean (from real data): an object's box is in fractions
of the board's width (y goes past 1 as boards scroll), mapped to a board
1,000 points wide; freehand and arrow points are fractions of the object's
own box. Arc's title object (`titleObjectID`, an h2 text at the top) is
left out, since the board shows its title itself. Ids: a board is
`ArcDerivedId("easels/<id>")`, an object
`ArcDerivedId("easels/<id>/objects/<oid>")`. Importing again never changes
a board that already has objects (it may have been edited or synced since);
an empty one gets Arc's. Arc's own edits it hadn't sent to its server yet
(Firestore's mutation queue) aren't read. Images that can't be fetched
become links (an image to its URL, a capture to its page).

## Later

- Sharing a board as a link, as Boosts are shared.
- Claude tools for boards (read a board, add objects).
