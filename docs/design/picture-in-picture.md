# Auto picture-in-picture

Status: done. Based on Chromium 156.0.8078.48.

## Goal

As in Arc: leaving a tab whose video is playing with sound shows the video
in a picture-in-picture window, and coming back to the tab hides it. If
several tabs play, the one left last shows. The window floats over every
app, including other apps' fullscreen spaces on macOS. Muted videos don't
show.

"Leaving" covers switching tabs, switching spaces, and swapping the window
to another profile's space. Leaving the window itself (another app, another
window, minimizing) doesn't count.

## Upstream does most of it

`AutoPictureInPictureTabHelper` (`chrome/browser/picture_in_picture/`)
already opens picture-in-picture when its tab stops being the active tab of
its strip, and closes it when the tab comes back. Out of the box it only
works for sites that register an `enterpictureinpicture` media session
handler, and only after a permission prompt or with high media engagement.
Compass changes five things:

| Upstream | Compass | Where |
|---|---|---|
| Browser-initiated auto pip (any page's playing video, no handler needed) is off | On | `compass::RegisterFeatureOverrides()` (`browser/feature_overrides.cc`), called from `ChromeBrowserFieldTrials::RegisterFeatureOverrides()`; `--disable-features=BrowserInitiatedAutomaticPictureInPicture` still turns it off |
| `AUTO_PICTURE_IN_PICTURE` content setting defaults to Ask: a prompt in the pip window, and no auto pip without high media engagement | Defaults to Allow: no prompt, no engagement check. Blocking a site still works | `content_settings_registry.cc` patch |
| Auto pip never replaces an open pip window | It replaces another tab's *auto* pip, so the tab left last shows. A pip window the user opened stays | `auto_picture_in_picture_tab_helper.cc` patch, `IsEligibleForAutoPictureInPicture()` |
| A tab moved into a popup window keeps its "active" state | A tab moved into the profile's parking window counts as left | `auto_picture_in_picture_tab_strip_observer_helper.cc` patch, `compass::IsInParkingWindow()` |
| "Back to tab" activates the tab in its `Browser` | A parked tab's space is shown in the window the user was in | `browser_web_contents_delegate.cc` patch, `compass::RevealParkedTab()` (below) |

The rest of upstream's conditions stay: the tab must have been audible
recently (so muted videos and muted tabs don't show), hold audio focus, pass
a Safe Browsing check of its URL (at most 500 ms), be https or file, and
have exactly one video player, at least 100×100 DIP, in its main frame. A
site that registers its own `enterpictureinpicture` handler gets that
instead (it may open document picture-in-picture).

## Spaces

A space switch (`WindowFeatures::SwitchToSpace()`) activates the new
space's tab before the old space's tabs move out, so the video tab sees an
ordinary tab switch. Coming back moves it in and activates it, which closes
the window.

A swap to another profile's space (`LeaveForSwap()`) detaches every tab
from the old window and parks it, with no activation change in a normal
strip. Upstream's tab strip helper ignores popup windows (the parking
window is one), so it would keep thinking the tab is shown. The patch
checks `compass::IsInParkingWindow()` on every tab strip change it sees
while the tab is away from a normal strip, and reports the tab as left.

## Back to tab

As in Arc, the window's "back to tab" button (and a page calling
`window.focus()`) shows the tab wherever it is. Upstream activates the tab
in its own `Browser`, which for a parked tab is the hidden parking window:
it would show that window. `BrowserWebContentsDelegate::ActivateContents()`
(patch) first asks `compass::RevealParkedTab()`: for a tab in a parking
window, the window the user was in last (of any listed profile) switches to
the tab's space, swapping to its profile if needed
(`SwitchWindowToSpace()`), and activates the tab, as a click on its entry
would. A favorite goes to its profile's active space. The parking window
never shows. Tabs in normal windows go upstream's way.

## Over fullscreen apps

`VideoOverlayWindowViews` is already a floating-level window visible on all
workspaces. On macOS a window shows in another app's fullscreen space only
if it is also a fullscreen auxiliary window, and only an activation-
independent ("nonactivating panel") window stays there when clicked. The
patch in `VideoOverlayWindowViews::ShowInactive()` sets both, as glic's
floating window does (`glic_floating_ui.cc`). Document picture-in-picture
windows (a `Browser` of their own) aren't changed.

## Click-through (macOS)

**Ctrl+Option+Cmd+P**, from any app, while a video's picture-in-picture
window is open, makes it ignore the mouse: clicks, scrolls and hovers go to
whatever is under it. While it does, the window fades to 25% under the
pointer (150 ms), so you can see what you're clicking. Its controls stay
hidden meanwhile: macOS still sends the window pointer moves, which would
show them (`VideoOverlayWindowViews::UpdateControlsVisibility()` asks
`PipWindows::IsClickThrough()`, patch). The same shortcut
makes it take clicks again; so does closing it (the next one takes clicks).

`compass::PipWindows` (`browser/ui/views/picture_in_picture/`, a
target of its own) tracks the windows that `VideoOverlayWindowViews::
ShowInactive()` (patch) hands it, until they hide (upstream hides a closed
window and may show it again) or are destroyed. It sets
`NSWindow.ignoresMouseEvents`, and registers the shortcut with upstream's
`GlobalAcceleratorListener` (a Carbon hot key, as extension commands use)
only while a window is open, so it takes the keys from other apps only
then. The window gets no mouse events while it ignores them, so a 50 ms
timer checks `NSEvent.mouseLocation` against its frame and animates its
alpha. Document picture-in-picture windows aren't covered.

The shortcut is fixed until shortcuts can be set (plan.md, item 14).

Test: `CompassAutoPipBrowserTest.ClickThroughShortcut` (not the hover fade,
which needs the real pointer).

## Gestures (macOS)

On the window, with a trackpad:

- **Pinch** resizes it around its center, keeping the video's aspect ratio,
  within upstream's minimum and maximum sizes (else the screen's work
  area), and kept on screen.
- **Two fingers** move it with the fingers, whichever way scrolling goes
  (`directionInvertedFromDevice`), kept inside the work area. Momentum
  after the fingers lift is ignored, so it stops where they leave it. A
  mouse wheel isn't a move.
- **Magnet:** when a pinch, a two-finger move or a mouse drag ends within
  32 pt of an edge of the work area, the window snaps to 12 pt from that
  edge (animated). A corner snaps on both sides.

`PipWindows` (was `PipClickThrough`) installs a local `NSEvent` monitor
for magnify, scroll and mouse-up events while a window is open, and takes
those aimed at a picture-in-picture window. The geometry is
`PipWindows::Zoom()`, `Move()` and `Snap()`. Not during click-through (the
window gets no such events).

Tests: `PipWindowsGeometryTest.*`, `CompassAutoPipBrowserTest.PipGestures`
(the geometry on the real window; the trackpad events themselves are
checked by hand).

## Window shape and behavior (macOS)

- **Corners:** `PipWindows` rounds the content view's layer (12 pt
  continuous corners, as the page card) and redraws the shadow to follow.
  Upstream's window had the standard frame (`NSWindowStyleMaskFullSizeContentView`)
  for its system corner radius; that frame also drew an outline, bright
  along the top in dark mode, that didn't follow the rounded content. It's
  borderless now (`video_overlay_window_native_widget_mac.mm` patch).
- **No macOS tiling:** `NSWindowCollectionBehaviorFullScreenDisallowsTiling`,
  so dragging it to a screen edge doesn't offer macOS's tiles; Compass's
  own snap (below) applies.
- **Closing doesn't bring Compass up:** a click on the window while another
  app is in front records that app; if the window then closes within 2 s
  (close, minimize), and Compass becomes active then or within 1 s, that
  app comes back to the front (`PipWindows::RestoreFrontApp()`). macOS gives
  the focus to Compass's main window when the clicked window goes. "Back to
  tab" is meant to bring the tab up and is left alone (`WillFocusOpener()`,
  patch). Not reproduced in tests (needs a real click while another app is
  active); a guess at the mechanism until checked by eye.

## Known gaps

- Leaving the window (another app or window, minimizing) doesn't show
  pip. Upstream's `kAutoPictureInPictureOnWindowOccluded` would add that.
- chrome://settings shows the auto picture-in-picture default as Allow,
  a choice its radio buttons (Ask, Block) don't offer.
- Plain http pages never auto pip (upstream rule).
