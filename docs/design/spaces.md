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
   is left for later.
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
outside macOS, where Ctrl+number picks tabs). `StrataSidebarView` registers
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
space's menu: its name, eight colors and "Delete space" (click twice). The
color (`SpaceColor`, saved by name as `"color"` in the file) tints the
sidebar's background and the space's button. Grey, the default, doesn't
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
(`SpaceList::Observer`). The palette is in both `GetSpaceTint()` and the
page's `SPACE_COLORS`, which still colors the space buttons and the menu's
swatches.

The tint fades over 280 ms (`gfx::LinearAnimation`, eased out, blending
premultiplied with `gfx::Tween::ColorValueBetween()`, so a grey space fades
the tint out rather than through black) whenever the window's space or its
color changes. While a swipe pulls in another space, the tint is the blend
of the two colors at the swipe's progress (next section), with no fade.

### Swiping

A two-finger horizontal swipe on the sidebar pulls in the next space
(fingers to the left) or the previous one, as in Arc. The page sees the
trackpad as wheel events (`onWheel_()`, non-passive on the app):

- The first event of a gesture picks its axis. Vertical gestures scroll as
  usual; a horizontal one swallows its events. A gesture ends after 150 ms
  without events; macOS momentum after the fingers lift is part of it.
- The window's name and entries (`#spacePane`, inside `#stage`, which clips
  them at the sidebar's edges) move with the fingers. The space being
  pulled in comes in next to them: a `<strata-sidebar-preview>`, which
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
  profiles), in `Strata Spaces.json` in the user data directory:
  `{"version": 1, "profiles": [<profile directory name>], "order": [<id>]}`.
  A space's entries stay in its profile's `SpaceList` and sidebar file.
- It loads the listed profiles when first used and holds a
  `ScopedProfileKeepAlive` for each (a new `kStrataSpaces` origin, patch): a
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

`SwitchWindowToSpace(browser, profile, space)` (window_features.h) rebinds
when `space` is the window's profile's. Otherwise:

1. The other profile's active space becomes `space`, so its next window opens
   there.
2. A normal window of that profile opens at the old window's bounds (its
   restored bounds, and maximized if the old one is), without session
   restore. Its `WindowFeatures` binds to `space`.
3. `WindowFeatures::EnterSpace()`, steps 3 to 5 of a switch: `space`'s
   parked tabs come in, and the entry it showed last is shown.
4. The new window shows and becomes key. It covers the old one exactly, so
   nothing flickers.
5. `LeaveForSwap()`: the old window's tabs leave as in step 6 of a switch,
   all of them, favorites' tabs included: to another window showing its
   space, else to its profile's parking window. Blank pages close. Its page
   stops taking requests and observing (`OnSwappedAway()`), and the emptied
   window closes on its own. If its space goes before it does, it takes the
   space its profile falls back to.

The page goes with its window, so a click on another profile's space swaps
at once, without a slide; the new window's sidebar opens on the space. A
swipe toward one runs as usual (its preview and tint come from that profile)
and swaps when it switches.

### Deleting

`DeleteSpace()` works across profiles: windows showing the space go to its
neighbor in the list, swapping if that's another profile's. Only the list's
last space can't be deleted. A profile whose last space goes leaves the list.

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

### Known gaps (slice 3)

- A fullscreen window swaps to a non-fullscreen one.
- The sidebar's width and collapsed state are per profile, so they can
  change on a swap.
- Assigning a profile reloads only the page the space showed; its other tabs
  come back unloaded, and their page state is lost.
- Entries can't move between spaces of different profiles.
- Spaces can't be reordered yet, so the list's order is the order they came.
- With several profiles, upstream may show its profile picker at startup.
- The swap has no animation of its own: the new window opens on its space.

## Known gaps (slice 1)

- A space with no name (only from files written before spaces were named)
  shows "•" in the switcher.
- Extensions see the parking window in `chrome.windows.getAll()`, as a
  popup.
- A parked page that calls `window.focus()` could show the parking window.
- Several windows on different spaces: see Startup.
- Only the sidebar is tinted; the window frame and toolbar keep the theme.
- Spaces can't be reordered.
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
