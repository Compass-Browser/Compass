# Spaces

Status: slices 1 and 2 done. Builds on ARCHITECTURE.md §6 (D6) and the
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
3. Spaces with different profiles.

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

## Known gaps (slice 1)

- A space with no name (only from files written before spaces were named)
  shows "•" in the switcher.
- Extensions see the parking window in `chrome.windows.getAll()`, as a
  popup.
- A parked page that calls `window.focus()` could show the parking window.
- Several windows on different spaces: see Startup.
- No animation on switch.
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
