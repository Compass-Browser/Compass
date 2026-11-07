# Window frame

Status: done. Based on Chromium 156.0.8078.48.

## Goal

As in Arc, the page sits in a rounded card inside a frame of the space's
color. With the sidebar expanded, the frame runs along the window's top,
trailing and bottom edges, and the sidebar is its leading part: one surface,
one color. Collapsed, the sidebar disappears: the frame goes all the way
round, the traffic lights hide, and hovering the frame's leading edge shows
the sidebar over the page, with the traffic lights.

## Measurements

`compass/browser/ui/views/frame/content_frame.h`, a header with no
dependencies, which upstream's layout and the region header include:

| Constant | Value | |
|---|---|---|
| `kContentFrameInset` | 8 DIP | The card's distance from the window's top, trailing and bottom edges |
| `kContentCornerRadius` | 12 DIP | The card's corners; with the inset, concentric with macOS's 20 DIP window corners |
| `kCollapsedSidebarWidth` | `kContentFrameInset` | A collapsed region is exactly the frame's leading edge |

## Layout

Upstream's `BrowserViewTabbedLayoutImpl` does the work, with a few patches
(`browser_view_tabbed_layout_impl.cc`):

- After placing the vertical tab strip region, it insets the rest of the
  client area by `GetContentFrameInsets()` (top, trailing, bottom), so the
  page, the top container, infobars and side panels sit inside the frame.
  The region itself provides the leading edge.
- The page's `MultiContentsView` gets `kContentCornerRadius` on every corner
  (`SetBackgroundRadii()`, which upstream only uses in glass mode), which
  rounds the active `ContentsContainerView` and clips the web contents as
  split view does. Not with horizontal tabs.
- No top separator over the card, and none of upstream's floating corner
  pieces (`vertical_tab_strip_top_corner`/`_bottom_corner`) that fake a
  rounded page corner beside the strip; the region's background draws no
  concave top trailing corner.
- The region has no outline along its trailing edge, except while it shows
  on hover over the page (with its shadow).
- The collapsed region starts at the top of the window, not under the
  caption buttons (`GetCollapsedVerticalTabStripRelativeTop()` is no longer
  used): the frame's edge runs the full height, and shown on hover the
  sidebar holds the traffic lights as when expanded.

## Collapsed

`VerticalTabStripRegionView::kCollapsedWidth` is `kCollapsedSidebarWidth`
(region header patch), so upstream's collapse animation, minimum size and
expand on hover all work with it. At that width the region hides its
children (region `.cc` patch, `CalculateProposedLayout()`): the header and
the sidebar page don't show as a sliver. The region's background and tint
look like the rest of the frame.

Upstream's expand on hover (on by default) shows the sidebar when the
pointer rests on the frame's leading edge, in the active window. Bubbles
anchored to any button in the header (the pill, site info, back with its
history menu, extensions, pinned actions, app menu) hold a `kKeepExpanded`
lock while shown, so the sidebar shows for them and they follow their
anchor there. Bubbles that upstream anchors to the hidden toolbar go to the
pill, collapsed too (`SidebarHeader::GetBubbleAnchorView()`). Page info and
permission prompts take upstream's fallback when nothing of the header is
drawn: a corner of the page, as in fullscreen.

`compass::WindowFrame` (owned by the region as a class property, installed
by `InstallSidebar()`) watches the region's bounds and fades the caption
buttons (`-[NSWindow standardWindowButton:]`'s `alphaValue`,
`window_frame_mac.mm`) with its width, from opaque at the expanded width
to hidden at the collapsed width, so they go and come with the sidebar's
slide (Cmd+S, the collapse button, hover). Not in fullscreen.

## Opening and closing

Upstream animates the region's width between expanded and collapsed (and
for expand on hover). Laying the sidebar out again at every width reflowed
its rows and squeezed the header. Instead, as in Arc, the sidebar slides:
while the width animates, the region gives its header and its contents
(the sidebar page) their expanded width, shifted left by what the region
lacks (`compass::GetSidebarSlideOffset()`, region `.cc` patch in
`CalculateProposedLayout()`), so they move out of the window's leading
edge, or in, at one size. The header is also measured at that width:
measured at the shrinking one, it took its taller collapsed layout (upstream
switches below `kCollapseSnapWidth`) halfway through and the page jumped
down. Closing, they also fade with the width
(`compass::FadeSidebarContents()`, from the region's `Layout()`); they get
a layer only while fading. The page card grows and shrinks with the region
as before.

## Traffic lights

The traffic lights and the sidebar header's top row (collapse, back,
forward, reload) are centered on one line, `kCaptionButtonsCenterY` (20 DIP
from the window's top). Upstream puts the lights 13 DIP from the leading
edge on macOS 26 and later, concentric with the window's corner; macOS
centers them in the title bar height the window reports, which upstream
takes from the toolbar Compass doesn't show (45 DIP, so 16 DIP down, 7 DIP
below the row). `compass::GetWindowTitlebarHeight()` reports 40 DIP instead
(patch in `BrowserNativeWidgetMac::GetWindowFrameTitlebarHeight()`, not in
fullscreen), which puts them 13 DIP down too, and `SidebarHeader` moves
upstream's row onto the same line.

## Color

The frame is the same color as the sidebar: a second `SidebarTint`, under
all of the browser view's children, paints the region's frame color
(`ThemedBackground`, kFrameTheme, which differs from what the frame view
paints) and the space tint over it. The region's own tint is scaled by its
background's opacity: with upstream's glass frame, the region's background
is transparent until it shows on hover, and the frame's tint shows through
it, so a full second tint would double it. Both tints fade and follow
swipes together; the window theme seed still comes from the space color
(spaces.md, "Window theme").

## Load bar

The active tab's load shows as a thin line (2 DIP, the sidebar's text
color at 70%) centered in the frame's gap above the card, as wide as the
card between its corners (`LoadBar`, a layered child of the browser view
kept over its siblings). While the tab waits for the server's first answer
(`WebContents::IsWaitingForResponse()`: connecting, or a slow server), a
segment a third as wide sweeps back and forth, 1.2 s a pass. Once the
server answers it becomes a line that eases toward the load's progress
(`GetLoadProgress()`), never back. When the load ends it fills, then fades
in 0.25 s. Switching tabs starts over with the new tab's state. There's
none in fullscreen, where the card has no gap above it. On Windows the
gap is under the caption buttons, and the line sits just above the card.

## Dragging

The frame drags the window like a title bar: `BrowserView::NonClientHitTest()`
(patch) returns `HTNOWHERE`, which the macOS frame view turns into
`HTCAPTION`, for points where `compass::IsOnWindowFrame()` finds no child of
the browser view but the tint. The collapsed region's strip stays client
area, for hovering.

## Windows

On Windows the caption buttons (minimize, maximize, close) belong to the
frame view (`BrowserCaptionButtonContainer`), under the browser view, at the
window's top trailing corner. Two things keep them in front of the frame
and the page:

- The frame's `SidebarTint` fills the whole browser view, so the container
  always paints to a transparent layer (patch), as upstream does only for
  web apps with Window Controls Overlay, and draws on top of the browser
  view.
- With no toolbar, the card would start 8 DIP from the top, under the
  buttons. Upstream passes the buttons' area to the layout as the trailing
  exclusion; `GetContentFrameInsets()` takes its height, so the card starts
  below the buttons and the frame runs across the top there like a title
  bar. On macOS the traffic lights are a leading exclusion (in the
  sidebar), so nothing changes there.

Points on that strip hit-test as on the rest of the frame (`HTNOWHERE` from
the browser view), so the Windows frame view checks its buttons first and
drags the window elsewhere. Linux's frame view has the same problem and
isn't fixed yet.

## Tests

Browser tests: `WindowFrame` (card insets and corners; the frame tint
matches the region's; the frame drags and the page doesn't; collapsed, the
region is the frame's edge, the card has the frame all round, and the
caption buttons hide until expanded),
`LoadBarSweepsWhileConnecting` (sweeps until the server answers, sits
above the card, gone after the load),
`CompassSidebarMotionBrowserTest.SlidesOutWithoutMovingDown` (steps the
close: the page keeps its y and size), `CollapsesAndResizes` (collapsed
width, the sidebar hidden and shown again). Header tests:
`RowLinesUpWithTrafficLights` (the row's center and the reported title bar
height); collapsed, no
controls or content setting icons are drawn; a bubble anchored to an
extension button or to the pill expands the collapsed sidebar; page info
falls back to upstream's corner while collapsed.

## Known gaps

- Expand on hover keeps upstream's delay (350 ms, or its velocity
  heuristic); Arc shows the sidebar at once at the edge.
- Collapsed, focusing the toolbar or the app menu from the keyboard
  (`IDC_FOCUS_TOOLBAR`, `IDC_FOCUS_MENU_BAR`) does nothing: the header
  isn't drawn, so its buttons can't take focus. F6 still reaches the page.
- The card has no shadow. Infobars, the bookmarks bar and side panels sit
  inside the frame but outside the card, with square corners.
- Right to left layouts aren't mirrored (the inset is on the right).
- The page card still lays out at every width of the animation (upstream's
  `MultiContentsView`), as in Arc.
