# Auto picture-in-picture

Status: done. Based on Chromium 156.0.8078.48. Calls: checked against live Teams and
Meet calls (macOS); Zoom not yet.

## Goal

As in Arc: leaving a tab whose video is playing with sound shows the video
in a picture-in-picture window, and coming back to the tab hides it. If
several tabs play, the one left last shows. The window floats over every
app, including other apps' fullscreen spaces on macOS. Muted videos don't
show.

"Leaving" covers switching tabs, switching spaces, swapping the window to
another profile's space, and leaving the window itself: covering it with
another app or window, or minimizing it (a setting turns that last one off;
"Leaving the window" below).

Calls go there too (Teams, Meet and Zoom on the web; "Calls" below): the
active speaker's video, with the call's microphone, camera and hang up
buttons, or the site's own mini window where it has one (Meet).

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
| Auto pip when the window is covered (`kAutoPictureInPictureOnWindowOccluded`) is off; a minimized window doesn't count | On, minimized counts, and a setting turns it off | `compass::RegisterFeatureOverrides()`; `auto_picture_in_picture_tab_helper.cc` patch, `OnOcclusionStateChanged()` ("Leaving the window" below) |

The rest of upstream's conditions stay: the tab must have been audible
recently (so muted videos and muted tabs don't show), hold audio focus, pass
a Safe Browsing check of its URL (at most 500 ms), be https or file, and
have exactly one video player, at least 100×100 DIP, in its main frame. A
site that registers its own `enterpictureinpicture` handler gets that
instead (it may open document picture-in-picture). A tab using the camera
or microphone with such a handler skips the playback conditions (sound,
audio focus, the single video): that is what calls go through.

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
windows (a `Browser` of their own, e.g. Meet's) get the same in
`PictureInPictureBrowserFrameView::AddedToWidget()` (patch). A
nonactivating panel can still be the key window, so their pages take typing
(Meet's chat), as glic's does.

## Click-through

**Ctrl+Option+Cmd+P** (**Shift+Alt+P** on Windows), from any app, while a video's picture-in-picture
window is open, makes it ignore the mouse: clicks, scrolls and hovers go to
whatever is under it. While it does, the window fades to 25% under the
pointer (150 ms), so you can see what you're clicking. Its controls stay
hidden meanwhile: macOS still sends the window pointer moves, which would
show them (`VideoOverlayWindowViews::UpdateControlsVisibility()` asks
`PipWindows::IsClickThrough()`, patch). The same shortcut
makes it take clicks again; so does closing it (the next one takes clicks).

`compass::PipWindows` (`browser/ui/views/picture_in_picture/`, a
target of its own: `pip_windows.cc` shared, `pip_windows_mac.mm` and
`pip_windows_win.cc` the platforms') tracks the windows that `VideoOverlayWindowViews::
ShowInactive()` (patch) hands it, until they hide (upstream hides a closed
window and may show it again) or are destroyed. It sets
`NSWindow.ignoresMouseEvents` on macOS, and `WS_EX_LAYERED |
WS_EX_TRANSPARENT` on Windows (a top-level window lets clicks through only
with both). It registers the shortcut with upstream's
`GlobalAcceleratorListener` (a Carbon hot key on macOS, `RegisterHotKey()`
on Windows, as extension commands use) only while a window is open, so it
takes the keys from other apps only then. The window gets no mouse events
while it ignores them, so a 50 ms timer checks the pointer against its
bounds and fades it: the window's alpha on macOS, the layered window's
(`SetLayeredWindowAttributes()`, stepped every 16 ms) on Windows. Document
picture-in-picture windows are tracked too
(`PictureInPictureBrowserFrameView::AddedToWidget()`, patch, as
`PipWindows::Kind::kDocument`): click-through, corners and snapping.

On Windows the default is Shift+Alt+P: upstream's Windows hot keys take
Shift, Ctrl and Alt only (a Windows key would be dropped), and Ctrl+Alt is
AltGr, which types characters on many layouts. For the same reason the
settings page refuses the Windows key for it, and `PipWindows` doesn't
listen for a shortcut that has it.

The keys can be changed in Keyboard shortcuts (shortcuts.md).

Test: `CompassAutoPipBrowserTest.ClickThroughShortcut` (not the hover fade,
which needs the real pointer). On Windows, whether DirectComposition's
content takes the layered window's alpha is to check by eye; the clicks go
through either way.

## Gestures

On the window, with a trackpad (macOS) or a precision touchpad (Windows):

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

`PipWindows` installs a local `NSEvent` monitor
for magnify, scroll and mouse-up events while a window is open, and takes
those aimed at a picture-in-picture window. The geometry is
`PipWindows::Zoom()`, `Move()` and `Snap()`. Not during click-through (the
window gets no such events).

A document picture-in-picture window keeps pinches and scrolls for its page
(`PipWindows::TakesGestures()`): it only snaps after a drag.

Tests: `PipWindowsGeometryTest.*`, `CompassAutoPipBrowserTest.PipGestures`
(the geometry on the real window; the trackpad events themselves are
checked by hand).

### Windows

Windows hands a precision touchpad's gestures to classic windows as wheel
messages, with no phase. `PipWindows` filters the window's messages
(`ui::HWNDSubclass`) while it's tracked:

- **Pinch** arrives as Ctrl+`WM_MOUSEWHEEL`: a notch's worth (120) resizes
  by 10%. Ctrl and a mouse wheel do the same.
- **Two fingers** arrive as `WM_MOUSEWHEEL`/`WM_MOUSEHWHEEL` in steps finer
  than a notch, which is how they're told from a mouse wheel (whose notches
  go on to upstream, which ignores them); a high-resolution mouse wheel
  counts as fingers. 120 units move it 100 DIP. The direction follows the
  touchpad's "Scrolling direction" (`ScrollDirection` under
  `HKCU\Software\Microsoft\Windows\CurrentVersion\PrecisionTouchPad`, 0 for
  "down motion scrolls down"; natural by default), read when the window
  shows.
- **End:** 150 ms without a wheel message ends a gesture and snaps the
  window. The touchpad's inertia can't be told from the fingers, so it
  moves the window on, unlike macOS; the snap waits for it to stop.
- **Magnet** after a drag or a resize: `WM_EXITSIZEMOVE` (the window is all
  caption, so a drag is the system's move loop). The snap is animated with
  a 200 ms ease-out.

Test: `CompassAutoPipBrowserTest.PipWheelGestures` (wheel messages sent to
the window). The real touchpad, and the direction's registry value, are
checked by hand.

## Window shape and behavior

macOS:

- **Corners:** `PipWindows` rounds the content view's layer (12 pt
  continuous corners, as the page card). The window has no system shadow:
  macOS draws a dark outline with it, which showed around the rounded
  window. The window keeps upstream's style
  (`NSWindowStyleMaskFullSizeContentView`): a borderless one left a
  title bar's height empty at its top. Its frame view drew a light line along the
  window's top edge, square to its corners: it's rounded and clipped too,
  and its title bar views (if any) hidden.
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

Windows:

- **Corners:** Windows 11's rounded corners
  (`DWMWA_WINDOW_CORNER_PREFERENCE` = `DWMWCP_ROUND`, 8 DIP, with the
  system's shadow); upstream asks for them only for windows with
  `rounded_corners`. Windows 10 keeps square ones.
- **Snap layouts** and closing are left as they are.

## Controls

`compass::PipStyle` (`browser/ui/views/picture_in_picture/pip_style.{h,cc}`),
macOS and Windows. It's built in upstream's overlay target (BUILD.gn patch),
whose `VideoOverlayWindowViews` names it a friend and calls it at the end of
`SetUpViews()` and when `OnUpdateControlsBounds()` returns
(`PipStyle::ScopedLayout`).

- **No site name or favicon.** The favicon and site name stay hidden (not
  the title view: upstream's fades show the views they fade, the title view
  and the scrims among them); back to tab says where the video comes from. Upstream shows the site name on
  untrusted pages so one can't pass itself off as another; Compass's
  windows are nearly all automatic ones of the user's own tabs. Upstream's
  timer for it is skipped too (`UpdateControlsVisibility()` patch).
- **Top buttons:** back to tab in the top left corner, with its title
  ("Back to tab", upstream's string for the button's tooltip) right of the
  icon, inside the button so a click on it goes back too; minimize and
  close side by side in the top right. 32 DIP high with 20 DIP icons
  (upstream: 24 and 16; button patches), `kMargin` (8 DIP) plus the pill's
  padding from the edges.
- **Pills.** Each group sits on a frosted pill: the top left and top right
  buttons, the playback buttons (or a call's camera, hang up and
  microphone) in the middle, and the timeline row at the bottom. A pill is
  dark and translucent (60%; the middle one, over the picture itself,
  30%), with the video behind it blurred, a faint light edge, and round
  ends. The middle one has 16 DIP at its sides, clear of its round ends. The
  pills are children of upstream's scrim, so they fade with the controls,
  and take no events.
- **Bottom row:** upstream's two rows (the timeline between previous and
  next track, the time under it on the left, mute and captions on the
  right) are one: previous, the time, the timeline, next, mute, captions
  (`LayOutBottomRow()`), `kMargin` from the window's edges. The time is as
  wide as its longest text (the current time as long as the duration), so
  the timeline doesn't move as it counts. It's drawn by `PipTime` over
  upstream's label (made transparent), each digit centered in a cell as
  wide as the widest digit and the text centered in its space: the system
  font's digits differ in width, and views can't ask for tabular ones, so
  the label's text shifted every second.
- **Timeline handle:** a 12 DIP circle, the played line running up to it
  (upstream: a 4x16 bar after a gap; `media_progress_view.cc` patch, so
  Chrome's other media controls have it too).
- **Dim.** A 20% black over the video while the controls show, instead of
  upstream's darker scrim and its black gradients at the top and bottom.
- **Space color.** The window's colors are seeded with the color of the
  space the tab belongs to (`Widget::SetUserColorOverride()`), as
  `SidebarTint` seeds a browser window's: the timeline and focus rings take
  it, and the pills get 20% of it. The space is the one whose sidebar lists
  the tab (`LiveTabRegistry`, `SpaceList::FindSpaceOf()`), so a tab parked
  for another space still has its own space's color; a tab the sidebar
  doesn't list (a little window's) takes its window's. None off the record
  or under an extension theme. `InstallPipTint()` (sidebar_tint.cc, from
  the sidebar view) provides it through `SetPipTintProvider()`
  (`pip_tint.h`, in the `pip_windows` target both sides depend on); each
  layout asks again, so a color change shows at the next one.

Test: `CompassAutoPipBrowserTest.PipStyle`.

## Leaving the window

Upstream's `kAutoPictureInPictureOnWindowOccluded` (on, in
`compass::RegisterFeatureOverrides()`) watches the active tab's window:
`AutoPictureInPictureWindowOcclusionHelperWebContents` on macOS and Windows
(the WebContents' visibility), `...Widget` on Linux. When it is covered
(another app or window over it), the tab's video or call goes into
picture-in-picture as if its tab were left; when it shows again, the window
closes. Upstream ignores a hidden (minimized) window; Compass treats it as
covered (`OnOcclusionStateChanged()`, patch). Switching to another app
whose windows don't cover Compass's doesn't count: the video is still in
view.

Settings → Privacy and security → Site settings → Automatic
picture-in-picture → "Also when you leave the window" (the
`compass.pip.on_leaving_window` profile pref, `compass::prefs::
kPipOnLeavingWindow`, default on; allowlisted for the settings page in the
`prefs_util.cc` patch, the toggle in the `auto_picture_in_picture_page`
patches). Off, the window being covered or minimized starts nothing; coming
back still closes a window opened before.

Tests: `CompassAutoPipBrowserTest.LeavingTheWindow`,
`LeavingTheWindowTurnedOff` (the WebContents made occluded or hidden).

## Calls

A call's tab has many videos: the user's own camera and a tile per
participant, all muted (the sound plays elsewhere). Upstream's
browser-initiated auto pip wants exactly one, with sound, so calls only go
into picture-in-picture where the site registers an `enterpictureinpicture`
handler: then a tab using the camera or microphone is eligible with no
other condition.

| Site | Window | How |
|---|---|---|
| Meet (`meet.google.com`) | Meet's own document picture-in-picture window (participants, its controls) | Meet's handler; Compass adds its window behavior (above) |
| Teams (`teams.microsoft.com`, `teams.live.com`, `teams.cloud.microsoft`) | Compass's video window: the active speaker | `call_pip.js` |
| Zoom web client (`*.zoom.us/wc/...`) | Compass's video window: the active speaker (Zoom draws video into canvases, captured) | `call_pip.js` |

`common/picture_in_picture/call_pip.js` is shared with iOS. On the desktop,
`compass::AttachCallPip()` (`browser/picture_in_picture/call_pip.cc`,
attached to every tab in the `tab_helpers.cc` patch) runs it in an
isolated world (`ISOLATED_WORLD_ID_CHROME_INTERNAL`) at `DOMContentLoaded`
of each frame of the shown page whose own URL is on the hosts above
(`IsCallPipURL()`; the script checks again): Zoom's web client runs the
call in a same-origin `<iframe>` of its `/wc/` page. Upstream routes the
tab's Media Session to the frame playing, so handlers registered there
count. It shares the page's DOM but not its JavaScript; Media
Session handlers are the page's, whichever world sets them. Once a second,
while a call is on (the site's hang up button and a remote tile exist), it:

- registers `enterpictureinpicture`. Upstream's auto pip calls it with a
  picture-in-picture request token, and it calls `requestPictureInPicture()`
  on the **pick**: the tile the site marks as speaking if a site entry says
  how, else the largest remote one shown (at least 100×100). Remote means a
  `<video>` playing a `MediaStream` whose video tracks aren't the user's
  own camera: not named as one of their cameras (`enumerateDevices()`;
  Chrome gives remote tracks a device id too, so that doesn't tell them
  apart, but names them otherwise, e.g. Teams' "remote video") and not a
  mirrored preview. Teams sets `disablePictureInPicture` on its tiles; the
  pick gets it cleared. On Zoom, the largest `<canvas>`,
  captured (`captureStream()`) into a hidden 1×1 `<video>` made ahead, as
  the video needs a frame before it can go into the window. In speaker view
  the largest tile is the speaker's;
- follows the speaker: while its video is in the window, another pick that
  stays the pick for 2 s takes its place (`requestPictureInPicture()` needs
  no token while the page has a video in picture-in-picture), in the same
  window;
- registers `togglemicrophone`, `togglecamera` and `hangup`, which click the
  site's own buttons, and reports `setMicrophoneActive()` and
  `setCameraActive()` from the buttons' labels ("Unmute", "Turn camera on",
  "Riattiva", ...: what pressing would do). Upstream's window then shows its
  microphone, camera and hang up buttons, which `PipStyle` puts in the
  centre pill;
- unregisters them all when the call ends, so other videos on the site go
  the usual way.

A site that registers one of these actions itself after the script keeps its
own. The site entries (button selectors, the speaking tile) hold what was
checked against live calls; they are the part to fix when a site changes.

Tests: `CompassCallPipBrowserTest.*` (a call page, with a fake camera and
canvas streams as remote tiles, served as `teams.microsoft.com`):
`LeavingShowsLargestTile`, `FollowsTheSpeaker`, `ButtonsPressTheSite`,
`CallInAFrame`,
`OtherSitesAreLeftAlone`, `CallSites`, and `SitesOwnWindow` (a page that
opens document picture-in-picture from its handler while using the
microphone, as Meet: tracked by `PipWindows` as a document window).

## Known gaps

- Calls: Teams' site entry matches a live call (Italian labels); Zoom's
  needs checking, and English labels on both. Entries break when a site
  changes its markup. Without
  a speaking marker, gallery view shows the largest tile, not necessarily
  the speaker's.
- Calls on Zoom: a canvas that draws the whole gallery shows the whole
  gallery. The host's "End" asks, in the page, whether to end the meeting
  for everyone. On iOS the call script runs in the main frame only, so
  Zoom's framed call isn't found there.
- Closing a window while its active tab plays may briefly ask for pip
  (the window hides before its tabs close); upstream drops the request with
  the tab.
- chrome://settings shows the auto picture-in-picture default as Allow,
  a choice its radio buttons (Ask, Block) don't offer.
- Plain http pages never auto pip (upstream rule).
