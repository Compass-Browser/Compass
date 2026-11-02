# Spaces

Status: slices 1, 2 and 3 done. Builds on ARCHITECTURE.md §6 (D6) and the
sidebar slices in [sidebar.md](sidebar.md). Based on Chromium 156.0.8078.48.

## Goal

A profile has several spaces. Each space is its own sidebar tree (Pinned
and Today). A window shows one space at a time, and a switcher at the bottom
of the sidebar moves the window to another space. Tabs belong to their
space and stay alive while it isn't shown.

## Slices

1. **Spaces in one profile**: several spaces, persisted, a switcher,
   switching a window between them.
2. **Editing spaces**: rename, colors, deleting a space, moving an entry to
   another space, keyboard shortcuts. A theme for the whole window per space
   came later (see "Window theme").
3. **Spaces with other profiles**: the switcher lists the spaces of every
   profile, and switching to one of another profile swaps the window.

## How D6 is realized

D6 says one `Browser` per (native window, space), with a switch swapping
which `Browser` the window shows. Read literally, that's several `Browser`s
in one native window, which `//chrome` can't host: a `BrowserView` is its
widget's client view. Its practical forms are:

- **Window group:** one real window per (window, space), all at the same
  bounds, one shown. A switch shows the target and hides the rest. Needs
  bounds, size, sidebar width and fullscreen kept in step across the group,
  flickers on macOS when the key window changes, and session restore
  can't tell which space a restored window belonged to without patches in
  three places. (`restore_id` is Ozone-only, and window "extra data" isn't
  rebuilt generically.)
- **Rebinding:** the window keeps its one `Browser`, and switching rebinds
  it to the other space. The live tabs of the space it leaves move out, and
  those of the space it enters move in, the way tab dragging moves tabs
  between windows. Page state, media and forms survive.

**Slice 1 rebinds (D15).** It needs no upstream changes, nothing flickers,
and window state stays in one place. Spaces with a different profile can't
share a `Browser`, so slice 3 adds the window group for those switches only.

## Where tabs of hidden spaces live

A tab leaving the window goes to another window that shows the same space,
if there is one, at the end of its strip and in the background. Otherwise
it goes to the profile's **parking window**: a `Browser` that's never shown.

- It's `TYPE_POPUP`, so upstream's `FindTabbedBrowser()` never picks it for
  links from other apps or new tabs.
- `omit_from_session_restore` keeps it out of the session, so it never
  comes back as a visible window.
- It has no `TabStripBridge`: the tabs keep their nodes in the
  `LiveTabRegistry`, and nothing mirrors them into a model.
- It's created on first use and closed when the profile's last normal window
  closes. Its tabs then close, and their entries stay, unloaded.

This is the hidden parking `Browser` of D8, limited to spaces that aren't
shown. D13's concern (a hidden window coming back through session restore)
is handled by the flag above.

Tabs of the space a window leaves that aren't worth keeping (new tab and
blank pages in Today, `IsRestorableURL()`) close instead of moving, with
their entries.

## Switching, step by step

`WindowFeatures::SwitchToSpace(target)`:

1. Note the strip's tabs: they belong to the space being left.
2. Drop the bridge to the old space.
3. Move the target space's parked tabs into the strip, in the background.
4. Create the bridge to the target space. Every tab already has a node, so
   it adds none.
5. Activate the target space's last active entry if it's live here, else
   its first live tab. If it has none, open its last active entry, its first
   entry, or a new tab, in that order.
6. Move the old space's tabs out, as above. The bridge doesn't put up a
   placeholder for them. Blank pages and placeholders close.

The strip never empties, so the window never closes on its own during a
switch.

## Model and persistence

- `SpaceModel` gains an ID and a name.
- `SpaceService` (per profile) holds the spaces in order, the registry
  (shared by all spaces, since node IDs are unique), a `RestoredTabMatcher`
  per space, each space's last active entry, and the **active space**: the
  space of the window used last. New windows open in it. The first space is
  "Home".
- The file becomes version 2:
  `{"version": 2, "active": "<id>", "spaces": [{"id", "name", "pinned", "today"}]}`.
  Version 1 files load as a single space.
- **Startup.** Windows open before the file has loaded, so they need a space
  right away. The service starts with one provisional space. When the file
  is in, the provisional space becomes the saved active space: it takes its
  ID and name, and its entries go in ahead. The other saved spaces are
  added. A restored window's tabs belong to the space it showed at quit,
  which is the active space when there's one window. With several windows on
  different spaces, the others' restored tabs land in the active space
  (known gap; fixing it needs a session patch).
- Closing a window hands its tabs only to a window of the same space.

## Moving entries

`MoveEntryToSpace()` (window_features.h), from dragging an entry onto
another space's button. `SpaceModel::MoveToSpace()` hands the node and its
subtree to the other model as they are: same objects, same IDs, so the
`LiveTabRegistry` keeps pairing them with their tabs. The source model's
observers see `OnNodeWillMoveToSpace()` (the page drops the entry, bridges
close placeholders for it, nothing closes tabs); the target's see
`OnNodeAdded()`. A pinned entry goes to the end of the other space's Pinned,
a Today entry to the end of its Today.

The live tabs then leave windows showing the old space, the same way a switch
moves them (step 6): to a window showing the new space, else to the parking
window. Parked tabs stay parked. A window whose active tab leaves first
activates the nearest tab that stays, or opens a new tab, so it never puts up
a placeholder for it.

## Deleting a space

`DeleteSpace()` (window_features.h). The last space can't be deleted. Every
window showing it (closing or not, so no bridge outlives its model) switches
to the space after it, or before it if it's last. Its live tabs are then all
in the parking window, where they close. Then `SpaceList::RemoveSpace()`
destroys it: the store, the service (matchers, last active entries) and the
pages hear `OnSpaceWillBeRemoved()` first.

## Shortcuts

Ctrl+1 to Ctrl+9 switch the window to the first nine spaces (Ctrl+Shift
outside macOS, where Ctrl+number picks tabs). `CompassSidebarView` registers
them with the window's focus manager, so they work while the sidebar is
collapsed. Pages get the keys first, as with upstream's own shortcuts.

## UI

The sidebar shows the space's name at the top, and at the bottom a row of
space buttons (the name's first letter) with a **+** that adds "Space N" and
switches to it. Mojo: `Page.SetSpaces(spaces, active_id)`, sent with the
tree and when the list changes; `PageHandler.SwitchSpace(id)` and
`AddSpace()`. When the window switches, the handler moves to the new space's
model and sends a fresh tree.

Right-clicking a space button, or double-clicking the space name, opens the
space's menu: its name, eight colors, a color well for any other color
(below) and "Delete space" (click twice). The color (`SpaceColor`, saved as
`"color"` in the file, see below) tints the sidebar's background and the
space's button. Grey, the default, doesn't
tint. `SpaceList` relays name and color changes of any space
(`OnSpaceUpdated()`), so every window's switcher follows. Mojo:
`RenameSpace()`, `SetSpaceColor()`, `DeleteSpace()`, `MoveNodeToSpace()`.

### Space colors

The tint is native, not the page's: `SidebarTint` (sidebar_tint.cc), the
first child of the vertical tab strip region, ignored by its layout, paints
the window's space color at 14% over the region's frame-colored background,
clipped to that background's shape, and follows the region's bounds. So the
header, the page (whose background is transparent) and the margins around
it are one color. A tint drawn by the page stopped at the page's edges: the
header above it and the region's margin below it stayed frame-colored.
`InstallSidebar()` adds it. It repaints when the window switches spaces
(`WindowFeatures::Observer`) or its space's color changes
(`SpaceList::Observer`).

A space can have any color, not only the palette's (2026-10-31, for Arc's
themes). `SpaceColor` is a value: grey (no tint) or an opaque `SkColor`.
The palette is its constants (`SpaceColor::kBlue`, ...) and
`kSpaceColorPalette`, mirrored by the page's `SPACE_COLORS`
(resources/sidebar/colors.ts) for the theme editor's palette. Mojo carries `SpaceColor? color` as
`{red, green, blue}`, null for grey. The file saves a palette color by name
(`"blue"`, so older builds still read it) and any other as `"#rrggbb"`;
a color it can't read stays grey. Any color is picked in the theme editor
(below): on its pad, or with its color well (an `<input type="color">`,
Chromium's color picker). While a color is dragged, the page passes one on
every 50 ms (`CUSTOM_COLOR_MS`), since each one reseeds the window's theme. Arc's
spaces come in with their theme's exact mid tone (arc-import.md).

The tint fades over 280 ms (`gfx::LinearAnimation`, eased out, blending
premultiplied with `gfx::Tween::ColorValueBetween()`, so a grey space fades
the tint out rather than through black) whenever the window's space or its
color changes. While a swipe pulls in another space, the tint is the blend
of the two colors at the swipe's progress (next section), with no fade.

### Gradients

A space with a color can go on to one or two more (2026-10-31, as Arc's
gradient themes): `SpaceModel::gradient()`, the colors after `color()`, at
most `kMaxSpaceGradientColors` (3) in all. Grey has none, and turning a
space grey drops it. The color stays the window's theme seed. Saved as
`"gradient": ["pink", "#010203"]` after `"color"`, left out when empty;
mojo `Space.gradient`, `SetSpaceGradient(id, colors)`. Moving a space to
another profile keeps it.

`SidebarTint` paints three stops evenly spaced (a plain color three times,
two colors with their blend in the middle), each at the tint's 14%, as one
linear gradient from the window's top left corner to its bottom right, in
widget coordinates: the region's tint and the frame's are two views but one
gradient, and both repaint when the window resizes. Fades and swipes blend
stop by stop, so a gradient fades into a plain color or another gradient. The
gradient is dithered: at 14% it spans only a few levels of each channel
across the window, and without dithering each level showed as a band 50-100
DIP wide (measured in a screenshot of the fast build: 34 changes along the
1600 px bottom edge, 895 with dithering).
The little window's frame keeps the space's color only. The space menu's
theme chip shows the same three stops, diagonally (`--space-color`,
`--space-color-2`, `--space-color-3`). The switcher's buttons show no color
(the user asked for no circle or background, 2026-10-31): just the icon,
emoji or letter, the window's space at full opacity, the others dimmed.
Symbols are 18px, letters 14px and emoji 11px (nudged up half a pixel),
which look about as big; even sizes sit on whole pixels in the 24px
buttons. The + is drawn (a "+" character sat 1.25px below the others).
Other spaces' emoji are a silhouette in the text color, as their symbols
and letters are (the glyph's fill transparent over a text shadow in
`currentColor`; `color: transparent` would make `currentColor` transparent
too), and fade into their colors in 240 ms when their space becomes the
window's.

In the space menu, a Gradient row under the swatches (hidden for grey):
each gradient color as a color well with ×, then + (while there's room),
which adds the last color turned 40° round the hue wheel. Wells pass colors
on every 50 ms while dragged, as the custom color well does. Picking a
swatch keeps the gradient.

Arc: a `blendedGradient` theme with two or more `baseColors` gives the
space its first base color as color and the next ones as gradient; other
themes the mid tone (arc-import.md). Intensity and grain: next section.

**Tests:** `SpaceModelTest.GradientsNeedAColor`,
`SidebarSerializerTest.RoundTripsGradients`, `PlainColorsSaveNoGradient`,
`SidebarPageHandlerTest.SetsSpaceGradients`,
`ArcSidebarTest.GradientsComeFromTheirBaseColors`, browser tests
`TintPaintsAGradient` and `ThemeEditorEditsTheSpace`.

### Intensity and grain

Two per-space settings from Arc's theme modifiers (2026-10-31), 0 to 1,
for plain colors and gradients alike (not grey): `SpaceModel::intensity()`
and `grain()`, clamped, NaN ignored. Saved as `"intensity"` and `"grain"`
only when not the defaults; mojo `Space.intensity`/`grain`,
`SetSpaceIntensity()`, `SetSpaceGrain()`; kept when the space moves to
another profile.

- Intensity: the tint's opacity is `kMaxSpaceTintOpacity` (110%) times
  it, capped at 100% (`SpaceTintOpacity()`), the phone's curve, so a synced
  intensity looks the same on both (2026-11-02; the user preferred the
  phone's stronger tint; it was 45%, so 14% by default). The default,
  `kDefaultSpaceIntensity` 0.31, gives about 34%. Arc's usual 0.8 is about
  88%; Arc's maximum is 1, as here. Flat over the window like Arc's (the
  phone had a fade to half by the bottom; the user dropped it on both). The
  window's theme seed doesn't change with it.
- Color: like Arc, the tint doesn't paint the space's color as it is but
  at 72% of its saturation and 4% darker (`ToTintColor()`, the phone's
  `RGB.tint`). Measured on the user's screen at full intensity (2026-11-02):
  Arc showed its stored #6cbad9 as #75aec4 in the sidebar, which this gives
  within a level. Only one intensity was measured, so the curve below 1 is
  ours. The theme seed stays the stored color.
- Text: in automatic mode, a tint strong enough to decide how light the
  frame is in both modes picks the window's mode (`GetTintColorMode()`, the
  phone's `SpaceTheme.colorScheme`), as Arc's `contentOverBackgroundAppearance`:
  the tint's average luma over a light (0.94) and a dark (0.13) frame, both
  above 0.5 → light, both below → dark, else the system's. The default
  intensity leaves the system's; a light color at full intensity is light
  (the user saw light grey text over the light blue Claudio in dark mode).
- Grain: a 128 px tile of grey noise (fixed sequence, built once) over the
  tint, in the tint's shape, blended as an overlay (grey 128 leaves the
  color, so hue stays), at up to 11.5% (`kMaxGrainOpacity`; 22% at first,
  which the user capped to 4.4% over the old 14% tint; raised on
  2026-11-02 to the noise measured in Arc at full grain and intensity,
  about 5 levels of luma; the phone's `SpaceTint.maxGrain` is the same). One grain per
  physical pixel, tiled in widget coordinates so the region's tint and the
  frame's line up.

Both blend in fades and swipes with the colors (`SidebarTint::Look`). The
menu has an Intensity and a Grain slider (0-100) under the gradient row,
hidden for grey, passing values on every 50 ms while dragged. Arc:
`modifiers.intensityFactor` as is, `noiseFactor` as grain when
`overlay` is `"grain"`, from `blendedGradient` or `blendedSingleColor`.

**Tests:** `SpaceModelTest.IntensityAndGrainStayInRange`,
`SidebarSerializerTest.RoundTripsIntensityAndGrain`,
`PlainColorsSaveNoGradient`, `SidebarPageHandlerTest.SetsSpaceIntensityAndGrain`,
`ArcSidebarTest.GradientsComeFromTheirBaseColors`,
`PlainColorsKeepTheirIntensity`, browser test `TintPaintsAGradient`.

### Empty pane

As in Arc, the empty pane (chrome://compass-empty, what a window with
nothing else to show shows) paints the space's tint rather than a plain
page (2026-11-02): the gradient's stops corner to corner over the frame
color (`--color-frame-active`), as opaque as the intensity says, and the
same grain tile at the same opacity, one grain per physical pixel. Its color
is Arc's pane shade, 66% saturation and 1.5% lighter (`TintSurface::kPane`;
Arc showed #6cbad9 as #82b6ca there, against #75aec4 in the sidebar). The
handler sends the page `EmptyPage.SetTint()` when the page binds, on a
space switch and on every change to the window's space, so the theme
editor shows live. No blend during swipes. A grey space keeps the page's
own background. Test: `CommandBarBrowserTest.EmptyPaneTakesTheSpaceTint`.

### Theme editor

The space menu's "Edit theme" row (with a chip of the space's colors)
opens the theme editor in the menu's place (2026-10-31, laid out after
Arc's, which the user shared a screenshot of): `<compass-theme-editor>`
(`resources/sidebar/theme_editor.{ts,html.ts,css}`). "‹ <name>" or Esc
goes back to the menu. It replaced the menu's swatches, color well,
Gradient row and sliders. From the top:

- Mode: automatic (sparkles), light (sun), dark (moon), hand-drawn
  glyphs. `SpaceModel::appearance()` (`SpaceAppearance`), saved as
  `"appearance": "light"|"dark"` (auto left out), mojo
  `SetSpaceAppearance()`. `SidebarTint::UpdateWindowTheme()` sets
  `Widget::SetColorModeOverride()`, so the browser's UI (frame, sidebar,
  menus, WebUI) takes that mode; web pages keep the system's. Off the
  record stays dark. Arc's equivalent wasn't found in its file, so imports
  stay automatic.
- The pad: the space's colors as dots, hue as the angle round the center
  (red at the top, clockwise) and saturation as the distance (to 42% of
  the pad's half size). The big dot is the color, the small ones the
  gradient. Dragging a dot keeps its lightness; dragging the big one turns
  and pushes the others by as much, so the gradient keeps its shape (Arc's
  "analogous" wheel). A press on the pad moves the big dot there; on a grey
  space it makes the first color, at 55% lightness. Arrow keys on a dot:
  hue ±5°, saturation ±5%. − and + under the dots: one color less or more
  (the new one 40° on from the last), 1 to 3.
- Palette: ‹ › between the colors (grey and the eight) and eight gradients
  (`GRADIENT_PRESETS` in colors.ts) plus a color well for any main color.
  A palette color replaces the gradient.
- The wave (intensity): a wave up to the thumb, growing with the value,
  flat after it; and the dial (grain): ticks lit up to the value, dragged
  up or right. Both are `role=slider` with arrow, Page and Home/End keys.

What's dragged shows at once (the editor's own copy until the browser's
space matches, or 600 ms after letting go) and goes to the browser every
50 ms. Color helpers moved to `resources/sidebar/colors.ts`.

**Tests:** `ThemeEditorEditsTheSpace` (palette, + and −, a dot's keys and
the big dot taking the gradient along, the gradients' page and any color,
the wave, the dial and dark mode, Esc),
`SidebarSerializerTest.RoundTripsAppearance`,
`SidebarPageHandlerTest.SetsSpaceIntensityAndGrain`.

### Space icons

A space can have an icon (2026-10-31, as in Arc), which its switcher button
shows instead of the name's first letter: an emoji or a symbol. The menu
lists them under the colors: "no icon" (the letter), 28 symbols and an
"Emoji" field that keeps the first character typed or picked (a whole
grapheme, so ZWJ sequences and flags stay one; macOS's Fn E / Ctrl Cmd
Space picker types into it).

`SpaceIcon` (space_model.h) is none, an emoji (non-empty UTF-8 of at most
64 bytes, no whitespace or controls) or a symbol name (`[a-z0-9-]`, at most
32). The handler drops invalid ones, as does the file reader. Saved as
`"icon": {"emoji": "🚀"}` or `{"symbol": "travel"}`, left out for none.
Mojo: `union SpaceIcon { string emoji; string symbol; }`, `Space.icon`,
`SetSpaceIcon(id, icon?)`. Moving a space to another profile keeps it.

The symbols are Compass's own set, `SPACE_SYMBOLS` in
`resources/sidebar/space_icons.ts`: Material Symbols paths (Apache 2.0) as
Chromium's WebUI iconsets carry them, all in the `0 -960 960 960` view box
so they share one style. Arc's own symbol artwork isn't copied: an Arc
symbol becomes the Compass one closest in meaning (`ArcSymbolToCompass()`:
planet → globe, github → code, colorPallete → palette, ...), or none.
Saved names must not change; a name the page doesn't know (a later
version's) shows the letter.

Not done: the icon elsewhere (command bar's space results, the little
window's space picker, the space name at the top).

**Tests:** `SpaceIconTest.*`, `SpaceModelTest.NotifiesIconChanges`,
`SidebarSerializerTest.RoundTripsIcons`, `UnreadableIconsAreLeftOut`,
`SidebarPageHandlerTest.SetsSpaceIcons`, `ArcSidebarTest.SpaceIcons`;
`SpaceMenuRenamesSetsIconsDeletes` picks a symbol and an emoji.

### Window theme

The space color also themes the window, as Arc does. `SidebarTint` sets it
as the window's theme seed with upstream's
`Widget::SetUserColorOverride()`, which `BrowserWidget::GetColorProviderKey()`
applies after the profile's theme (as an accent color). Chrome then derives
the window's colors from it as it does for a profile's "Customize Chrome"
color: the frame and the region's background, the header's pill, menus,
bubbles and the WebUI pages in the window. No patch is needed. Grey clears
the override, so the window has the profile's theme. Off-the-record
windows and profiles with an extension theme keep their own colors.

It's set when the tint is added to the window, when the window switches
spaces and when its space's color changes. Not during a swipe: a new seed
rebuilds every color in the window, too slow for each wheel event, so the
swipe blends only the tint, and the theme changes when the switch lands.
The tint stays on top, 14% of the color over the themed region.

The sidebar, command bar and placeholder pages read the theme through
`chrome://theme/colors.css`, built from their WebContents' color provider,
which is the window's. They start upstream's `ColorChangeUpdater`, which
reloads that style sheet when the window's colors change; without it the
page kept the colors it loaded with (a grey active row on a blue sidebar).

Upstream's tab group "focus mode" sets the same override from the tab strip
controllers. Compass doesn't mirror tab groups, so it doesn't clash.

**Tests:** `SidebarSerializerTest.SavesCustomColorsAsHex`,
`UnreadableColorsStayGrey`; `ThemeEditorEditsTheSpace` (palette swatch,
then the color well). `WindowThemeFollowsTheSpace`: the override and the frame color
follow the space's color and a switch, not a swipe; grey goes back to the
profile's frame color; the sidebar page's colors change with it; an
off-the-record window isn't themed.

### Swiping

A two-finger horizontal swipe on the sidebar pulls in the next space
(fingers to the left) or the previous one, as in Arc. The page sees the
trackpad as wheel events (`onWheel_()`, non-passive on the app):

- The first event of a gesture picks its axis. Vertical gestures scroll as
  usual; a horizontal one swallows its events. A gesture ends after 150 ms
  without events; macOS momentum after the fingers lift is part of it.
- The window's name and entries (`#spacePane`, inside `#stage`, which clips
  them at the sidebar's edges) move with the fingers. The space being
  pulled in comes in next to them: a `<compass-sidebar-preview>`, which
  renders a tree from `PageHandler.GetSpaceTree()` in its own shadow root,
  inert. The previous and the next space's previews are always built,
  waiting just past the stage's edges (`side`), so a swipe never shows one
  empty or loading favicons. The page refetches every other space's tree
  when the spaces, the window's tree or its entries' set change, and when a
  swipe starts; the space just left keeps the tree it showed. The previews
  change only between slides. They show what the switch will: the entry it
  will show is highlighted (`PredictShownEntry()`, step 5 above, sent as
  the tree's `active_id`), and a space with no entries shows the new tab
  the browser will open there, since blank tabs close when the window
  leaves a space. Past the first or the last space the entries
  only give a little (rubber band, at most 32 px).
- Each frame, `PageHandler.PreviewSpace(id, progress)` tells
  `WindowFeatures::SetSpacePreview()`, and `SidebarTint` paints the blend.
- Half the width switches at once; a gesture that stops past a quarter
  switches too, and one short of it goes back (and the tint fades back).
  Switching animates the rest of the way, calls `SwitchSpace()`, and
  swallows the rest of the gesture. Once the new tree has arrived and the
  preview is in place, the live pane, now showing it, takes the preview's
  place. A switch the browser hasn't confirmed in 2 s is undone.

A click on a space button slides the same way at once, from zero: a space
further away than a neighbor first takes that side's preview, from its
cached tree. In both cases the page drives the tint through
`PreviewSpace()` frame by frame along the slide, and calls `SwitchSpace()`
once the slide's first frame is out, since switching keeps the browser busy
for a moment; once the browser has switched, it fades the tint itself.

Switches from elsewhere (Ctrl+1–9, a new or deleted space) slide too: when `SetSpaces()` names another active space, the page waits for
its tree; then a copy of the old pane (in a shadow root of its own, like the
FLIP copies in sidebar.md, "Motion") slides out while the live pane slides
in, from the right for a later space and from the left for an earlier one.
The favorites stay put: they're the profile's. `prefers-reduced-motion`
turns the slides off, and `gfx::Animation::ShouldRenderRichAnimation()` the
tint's fade.


## Slice 3: spaces with other profiles

As in Arc, the switcher is one list of spaces in the user's order, and each
space is assigned to a profile: its tabs, cookies, history and favorites are
that profile's. A window can switch to any space. Switching to a space of the
window's own profile rebinds, as before. Switching to one of another profile
swaps the window.

### The list

- `SpaceDirectory` (`browser/window_sync/space_directory.h`), browser-wide,
  keeps the order of the spaces and which profiles have spaces (listed
  profiles), in `Compass Spaces.json` in the user data directory:
  `{"version": 1, "profiles": [<profile directory name>], "order": [<id>]}`.
  A space's entries stay in its profile's `SpaceList` and sidebar file.
- It loads the listed profiles when first used and holds a
  `ScopedProfileKeepAlive` for each (a new `kCompassSpaces` origin, patch): a
  profile whose last window swapped away would otherwise be destroyed
  (`kDestroyProfileOnBrowserClose`), with its parked tabs.
- A profile is listed once a normal window of it opens (from upstream's
  profile menu, say) or a space is assigned to it, and stops being listed when
  its last space goes. New spaces join at the end of the list; a space moved
  to another profile keeps its place. Spaces a profile's file brings in join
  once the file has loaded.
- Off-the-record, guest and system profiles are never listed. Off-the-record
  windows list their own spaces only.

### Assigning a profile

The space menu has a Profile picker: every profile upstream's profile menu
lists, and "New profile…", which creates one
(`ProfileManager::CreateMultiProfileAsync()`). `AssignSpaceToProfile()`
waits for the profile and its sidebar file to load, then
`MoveSpaceToProfile()`:

1. A space of the new profile with the same name, color, archive delay and
   archive goes right after the old one in the list.
2. The old space's live tabs can't change profile. They're unregistered and
   close, and their entries stay, unloaded.
3. Its entries move to the new space: the same nodes, as between spaces of a
   profile (`SpaceModel::MoveToSpace()`), with its last active entry.
4. Windows showing it swap to the new space (below). The new window loads
   the entry the space showed, so the page the user was on reloads in the
   new profile.
5. The old space goes. If it was its profile's last, the profile leaves the
   list, and gets a fresh "Home" for when a window of it opens again.

A profile new to the list (a new profile's) comes with the assigned space
only: the empty spaces it starts with are dropped.

### Swapping the window

A `Browser` belongs to one profile for life, and a tab can't move to
another profile's window, so a `Browser` can't change profile. A swap
builds the other profile's `Browser` and gives it the native window (D18).

`SwitchWindowToSpace(browser, profile, space)` (window_features.h) rebinds
when `space` is the window's profile's. Otherwise:

1. The old window begins swapping (`BeginSwap()`): its page stops taking
   requests and observing (`OnSwappedAway()`), frozen where its slide ended,
   and it can't swap again. The other profile's active space becomes `space`,
   so its next window opens there.
2. A normal window of that profile opens at the old window's bounds, in its
   state (maximized or fullscreen), without session restore, with the old
   window's sidebar width and collapsed state. Its `WindowFeatures` binds to
   `space`.
3. `WindowFeatures::EnterSpace()`, steps 3 to 5 of a switch: `space`'s
   parked tabs come in, and the entry it showed last is shown.
4. The swap presenter (`sidebar_swap.cc`, set by the views layer) never
   shows the new window. Once its sidebar page has the tree, the two
   windows' `BrowserView`s exchange native windows
   (`compass::BrowserViewExchange`): the shown NSWindow now holds the new
   profile's view, and the old view goes to the hidden one. The shown window
   keeps its last frame (a compositor lock) until the sidebar and the active
   page have painted, at most 750 ms. Same NSWindow, macOS Space and
   fullscreen. See docs/design/profiles-in-place.md.
5. `LeaveForSwap()`: the old window's tabs leave as in step 6 of a switch,
   all of them, favorites' tabs included: to another window showing its
   space, else to its profile's parking window. Blank pages close. The
   emptied window closes on its own. If its space goes before then (it was
   moved to another profile), its bridge goes, and it takes the space its
   profile falls back to.

In the page, a click on another profile's space slides like any other and
asks for the switch as the slide starts. The window keeps the page
(profiles-in-place.md, "One sidebar page"): the new window's handler sends it
the space's tree mid-slide, as within a profile, so the slide runs on in one
page. A swipe works the same way. The tint blends toward the new space's
color along the slide; the new window's tints go on from where the old
window's were.

### Deleting

`DeleteSpace()` works across profiles: windows showing the space go to its
neighbor in the list, swapping if that's another profile's. Only the list's
last space can't be deleted. A profile whose last space goes leaves the list.

### Reordering

Dragging a space button along the switcher moves the space: it drops before
the button it's over (the button's leading half) or after it, and a bar in
the gap shows where. The slots next to the space itself show nothing and
don't move it. The page calls `MoveSpace(id, index)`, the slot counted with
the space still in place, as `MoveNode()` does for entries. In a listed
profile's window it moves in `SpaceDirectory`'s order (any profile's space,
saved in `Compass Spaces.json`); the profile's own `SpaceList` keeps its
order. Off the record, it moves in the profile's `SpaceList`
(`SpaceList::MoveSpace()`, saved in the sidebar file). Every window's
switcher follows, the buttons glide to their places (FLIP, as entries do),
and Ctrl+1–9, swipes and slide directions follow the new order. Spaces of
listed profiles that aren't loaded keep their places in the saved order.

### Parking across profiles

A profile's parked tabs stay as long as any window can switch to them:
parking windows close when the last normal window of every listed profile
closes, not their own profile's last. Closing a window still hands its tabs
only to a window of the same space, and closes them if there's none.

### UI

`Space` (mojom) has `profile_id`, `profile_name` and `own_profile`, and
`SetSpaces()` also sends the profiles a space can be assigned to (none off
the record), refreshed when profiles are added, renamed or removed. A
button's tooltip names the profile once the list has more than one. "+" adds
a space of the window's profile. An entry can't move to another profile's
space (its tab can't): dragging onto such a button shows no drop target, and
"Move to Space" offers only the profile's own spaces. Swipe previews and the
tint work for any space. Ctrl+1–9 follow the list. The command bar offers
every listed space, but only the window's profile's entries.

### Startup with several profiles

There's no profile picker at startup (`BrowserProfilePickerAvailabilityOnStartup`
defaults to disabled, `compass/browser/prefs/pref_defaults.cc`; policy can
still turn it on). The first window opens in the last used profile, as
upstream does without the picker. When the switcher lists spaces of more than
one profile, that window then shows the switcher's **first** space, in its
profile (`compass::MaybeShowFirstSpaceAtStartup()`, `startup_space.cc`):

- Only the process's first normal window, and only when the session isn't
  restored ("Continue where you left off" brings back the spaces the windows
  showed).
- It waits until the directory file and every listed profile's spaces are
  in (`SpaceDirectory::IsLoaded()`), at most 10 seconds, then calls
  `SwitchWindowToSpace()`: a rebind for a space of the window's profile, a
  swap for another profile's.
- With one profile the window keeps the space used last.

### Known gaps (slice 3)

- To extensions and the session, the window is another window after a
  switch to another profile (a new `windowId`): it is that profile's.
- Assigning a profile reloads only the page the space showed; its other tabs
  come back unloaded, and their page state is lost.
- Entries can't move between spaces of different profiles.
- With several profiles, upstream may show its profile picker at startup.

## Known gaps (slice 1)

- A space with no name (only from files written before spaces were named)
  shows "•" in the switcher.
- Extensions see the parking window in `chrome.windows.getAll()`, as a
  popup.
- A parked page that calls `window.focus()` could show the parking window.
- Several windows on different spaces: see Startup.
- The theme changes at once when the window switches spaces; only the
  tint fades. A change of the profile's theme to or from an extension theme
  applies at the next switch or color change.
- Deleting a space closes parked tabs without asking, even ones with
  beforeunload handlers.

## Tests

Serializer and store tests for version 2, version 1 files and the
provisional space; `WindowFeatures` tests for switching there and back (tabs
live in parking, the same tabs come back, blank tabs close), a switch to an
empty space, and hand-off between windows of different spaces; page handler
tests for the space list and the fresh tree after a switch; a browser test
that clicks a space button.

Slice 2: model tests for moving between spaces and colors, `SpaceList`
removal and relayed changes, store round trips of colors, removals and moves,
handler tests for rename, color, moving an entry with its tab and deleting
(the window's own space, another one, not the last), and browser tests for
the menu, dragging onto a space and the shortcuts.

Slice 3: `SpaceSwapTest` (one list in order, profiles listed by windows,
moving a space to a profile reloading it there and keeping its place,
deleting a profile's last space, deleting the shown space swapping to its
neighbor, off-the-record windows, swapping and back, parked tabs outliving
their profile's last window), page handler tests (listing, previews, entries
kept from other profiles, swapping, assigning a profile), and the
`AnotherProfilesSpaceSwapsTheWindow` and `SpaceMenuAssignsAProfile` browser
tests.

Reordering: `SpaceListTest.MovesSpacesBySlot`,
`SpaceSwapTest.ReordersSpacesAcrossProfiles` (and the saved order),
`SidebarPageHandlerTest.ReordersSpacesAcrossProfiles`, and the
`DraggingASpaceReordersThem` browser test (drop marker, a no-op slot,
Ctrl+1 following the order).
