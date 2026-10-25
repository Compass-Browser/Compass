# Command bar and toolbar-free window

Status: slices 1 to 4 done (command bar, sidebar header, no toolbar,
Favorites). Builds on ARCHITECTURE.md §4 and §11 and on
[sidebar.md](sidebar.md). Based on Chromium 156.0.8078.48.

## Goal

Arc's window has no toolbar. The sidebar's top holds the traffic lights,
the sidebar toggle and back, forward and reload, with a URL pill under
them. Typing a URL or a search happens in the command bar, a box that floats
over the page: Cmd+T opens it to make a new entry, and Cmd+L (or a click on
the pill) opens it on the current page's URL.

## Slices

1. **Command bar**: the floating box with omnibox suggestions, sidebar
   entries, spaces and commands. Cmd+T, Cmd+L and the sidebar's New Tab
   row open it. The toolbar is still there.
2. **Sidebar header**: traffic lights, sidebar toggle, back, forward and
   reload in the region's top row; the URL pill (host of the active tab)
   under it, which opens the command bar on the current URL.
3. **No toolbar**: the window's toolbar is hidden. Site info and an
   extensions button go on the pill, pinned extension icons in a row under
   it, downloads in a popover at the top of the sidebar, and the app menu
   and profile in a ⋯ menu. Bubbles that anchored to the toolbar anchor to
   those.
4. **Favorites**: a grid of large icon tiles above the space name, shared
   by every space of the profile.

## Slice 1: command bar

### Hosting

The bar is `chrome://strata-command-bar`, a top-chrome WebUI shown with
upstream's `WebUIBubbleManager`, as tab search is. That gives us a page
kept alive between openings (open is instant after the first), a bubble
sized to the page, focus handling and Esc to close. Each window has one
`CommandBar` (`browser/ui/views/command_bar/`), owned by `WindowFeatures`'s
view side: it shows the bubble centered over the top of the content area,
with no arrow, and pushes the mode and the starting text to the page when
it opens.

`WebUIContentsWrapperT` checks the controller's name against the
`WebUIName` histogram variants, so a one-line patch adds
`.StrataCommandBar` there.

### Opening it

`BrowserCommandController::ExecuteCommandWithDispositionAndContext()` gets
a hook (patch) that offers each command to `strata::HandleBrowserCommand()`
first. It sits there, not in `HandleCommandWithDisposition()`, because with
upstream's `UseActionsForBrowserCommands` the command runs as a browser
action and never reaches the latter. It takes:

| Command | Mode | Starting text |
|---|---|---|
| `IDC_NEW_TAB` (Cmd+T, File menu) | new entry | empty |
| `IDC_FOCUS_LOCATION` (Cmd+L) | current entry | the active tab's URL, selected |

Code that calls `chrome::NewTab()` directly (session restore, tests, a
window with no tabs) still gets a new tab page. The sidebar's New Tab row
opens the bar in new-entry mode. Dismissing the bar does nothing: no empty
tab is left behind.

### Results

`CommandBarPageHandler` runs one query at a time and sends one sorted list
per change:

- **Omnibox suggestions** from an `AutocompleteController` with the default
  omnibox providers, as `chrome://omnibox` builds it. Results come in
  several passes; each one resends the list. The default match is first,
  and Enter takes it. Its inline autocompletion is shown selected in the
  field, as in the omnibox.
- **Entries**: favorites and tab nodes of the profile's spaces whose title
  or URL contains every word of the query, at most five: favorites first,
  then the window's space, then the others. Shown as "Switch to". Choosing
  one in another space switches the window there first; choosing a
  favorite opens or activates it in this window, as a click on its tile
  does.
- **Spaces**: whose name contains the query. Choosing one switches to it.
- **Commands**: a fixed list (new space, pin or unpin the current entry,
  add it to or remove it from Favorites, copy URL, reload, history,
  downloads, extensions, settings), matched by name. Commands that need an
  entry are left out when there's none. Pin and unpin are left out for a
  favorite. "Add to Favorites" puts the entry at the end of the grid (a
  pane leaves its split); its tab stays. "Remove from Favorites" moves it to
  the top of the window's Today, as unpinning does.

Order: the default omnibox match, entries, the other omnibox matches,
spaces, commands. With an empty query in new-entry mode the list is the
window's Today entries, most recent first, then the commands.

### Accepting

- An omnibox match navigates to `match.destination_url` with the match's
  transition (plus `FROM_ADDRESS_BAR`): in a new foreground tab (a new
  Today entry) in new-entry mode, in the active tab otherwise. The typed
  text and match go to the profile's `ShortcutsBackend`, so the shortcuts
  provider learns from it as it does for the omnibox; history learns from
  the navigation itself. An `OmniboxLog` goes to the
  `OmniboxEventGlobalTracker` (omnibox metrics, the extensions omnibox API)
  and the `AutocompleteActionPredictor`, as `OmniboxEditModel::OpenMatch()`
  sends it: the text, the selected line in the controller's result, the
  disposition, the page classification, the time since the first edit
  after opening and since the default match last changed (-1 ms, "unknown",
  when the user didn't edit), and the providers' logs. Search prefetch and
  prerender aren't used, so their part of the log is skipped. A stale ID
  (the page behind, see `Accept()`) has no result to log and reports
  nothing.
- Cmd+Enter on an omnibox match opens it as a new entry in either mode.
- An entry is activated (opened if unloaded) in this window.

The bubble closes after any accept, and when the window loses focus.

### Pointer

Hovering a row selects it, but only after the pointer has really moved
while the bar is open. Blink sends a `pointermove` without movement when
the rows change under a resting pointer (the bubble appearing under it, a
new pass of results), and that mustn't take the selection away from the
default suggestion. The page remembers the pointer's screen position from
the first event after opening; a move selects only from a different
position, and only with a nonzero `movementX` or `movementY`.

### Tests

`CommandBarPageHandlerTest` (unit): result order for empty and typed
queries, other spaces' entries, stable IDs within a query, accepting each
kind of result, including a stale ID, and the omnibox log of an accepted
suggestion. `CommandBarBrowserTest`: Cmd+T then a URL and Enter makes a
Today entry, Cmd+L starts from the page's URL and loads in place, opening
again starts over, Esc closes it without a new tab, and a pointer event
without movement doesn't change the selection. The Esc test hands the key
to the contents' delegate (the bubble's wrapper), which closes it before
the renderer sees it; `SimulateKeyPress()` never reached it with the test
window in the background.

### Known gaps

- The hook sits in
  `BrowserCommandController::ExecuteCommandWithDispositionAndContext()`,
  which menus and accelerators go through whether or not upstream runs
  commands as browser actions. Anything that invokes the
  New Tab action item directly (upstream's app menu redesign, if enabled)
  still opens a plain new tab.

## Slice 2: sidebar header

### Where it lives

Upstream's vertical tab strip region already has a top row: the traffic
lights (as the caption-button exclusion), the collapse button and the combo
button (Everything menu and tab search). Its layout,
`VerticalTabStripTopContainer::CalculateProposedLayout()`, is written out
per button, so adding buttons to it would be a large patch that conflicts on
every update. Instead `strata::SidebarHeader` subclasses the container and
overrides that method: it takes upstream's layout and adds its own views
around it. The region patch constructs it in place of the upstream class
(one line).

The header's views are native, not part of the sidebar page: slice 3 puts
site info and the extensions button on the pill, and their bubbles anchor
to views.

### Contents

- **Back, forward, reload**: upstream's `BackForwardButton` (with its
  history menu on long press or right click) and `ReloadButton` (with its
  stop mode and menu), wired as `ToolbarView` wires them: back and forward
  run `IDC_BACK`/`IDC_FORWARD`, and the header observes the commands'
  enabled state. Reload switches to stop while the active tab loads.
- **URL pill**: a full-width button under the row showing the active tab's
  host (`www.` dropped), or its full URL for schemes without a host, and
  "Search or enter URL" for a new tab page. Clicking it opens the command
  bar on the current URL, as Cmd+L does.

### Layout

When the region is collapsed the header adds nothing and upstream's layout
stands. Expanded:

- The nav buttons sit at the right end of the top row, centered on the
  collapse button's center, to the left of the combo button if that has
  any visible part. If they don't fit between the collapse button and that
  edge, they move to a row of their own under upstream's layout.
- The pill takes a row under everything else, with the region's horizontal
  padding.

The combo button's two parts (Everything menu, tab search) are user prefs.
Strata defaults both to unpinned (tab_strip_prefs patch): the command bar
covers tab search. Pinning one again from its context menu brings it back
next to the nav buttons.

The toolbar is still shown in this slice; slice 3 hides it.

### Tests

Browser tests: the header shows the buttons and the pill in the expanded
region and hides them collapsed; back becomes enabled after a navigation
and goes back; the pill follows the active tab's host; clicking it opens
the command bar on the current URL.

## Slice 3: no toolbar

### Hiding it

`BrowserView::IsToolbarVisible()` asks `strata::ShouldHideToolbar()` first
(patch). It returns true when the window's `SidebarHeader` holds the
toolbar's controls (see below), which is only in vertical tab mode. The
tabbed layout then gives the toolbar no space, as it does in fullscreen. The
`ToolbarView` is still built and keeps running: the location bar's model,
page actions, the extensions and download controllers all live there, and
the rest of the browser reaches them through `ToolbarButtonProvider`.

The location bar can't take focus while the toolbar is hidden (upstream's
`UpdateFocusBehavior()`). Cmd+L opens the command bar anyway.

### Moving the controls

Rather than rebuilding them, the header takes upstream's own views out of
the toolbar and adds them to itself. Bubbles that anchor to them (the
extensions menu, extension popups, the downloads bubble, the app menu) then
appear in the sidebar with no further change:

| View | Found by | Goes to |
|---|---|---|
| Extensions container (pinned extensions and the extensions button) | `kToolbarExtensionsContainerElementId` | the pill's row, or a row under it |
| Pinned toolbar actions (downloads while there are any, actions the user pinned) | `kPinnedToolbarActionsContainerElementId` | next to the extensions |
| App menu button | `VIEW_ID_APP_MENU` | the end of the pill's row |

The profile menu is a submenu of the app menu, so the avatar button stays in
the hidden toolbar. Everything else in the toolbar (home, the split tabs
button, media, battery saver) stays hidden too.

The toolbar's own code keeps its pointers to the moved views. Its
`FlexLayout` only lays out its own children, and its responsive overflow
controller skips elements it can't find among them.

Timing: the header exists before the toolbar is initialized
(`BrowserView::AddedToWidget()` calls `ToolbarView::Init()` after its
children were notified), so `BrowserView` calls `strata::AttachToolbar()`
right after `Init()` (patch). The views go back to the toolbar, at their
old index, in two cases: when the window switches to horizontal tabs (the
header listens to `RegisterOnModeChanged()`), and at the start of
`~BrowserView()` through `strata::DetachToolbar()` (patch). The latter is
needed because `BrowserView` deletes the top container (and the toolbar,
which has members that use these views) before the vertical region.

### Site info

A button at the pill's left shows the page's security icon (the location
bar model's `GetVectorIcon()`) and opens page info anchored to itself. It
builds the bubble as `ShowPageInfoDialogImpl()` does, with its own anchor.
It's hidden on the new tab page, which has no page info.

### Where other bubbles go

- **Permission prompts** use `GetPageInfoAnchorConfiguration()`: with the
  location bar not drawn it falls back to the app menu button, which is
  now in the sidebar next to the pill.
- **Page action bubbles** (bookmark star, passwords, translate, zoom,
  cookie controls, ...) go through `ToolbarView::GetBubbleAnchor()`, which
  anchors to the top container when the location bar isn't drawn: the top
  right of the page. Arc shows these there too, so no patch for now.

### Layout

Expanded: the pill row is `[pill][extensions][pinned actions][app menu]`.
If that leaves the pill narrower than 120 DIPs, the extensions and pinned
actions move to a row of their own under the pill, aligned right. Collapsed:
only the app menu shows, centered in a row under upstream's layout, so the
menu stays reachable; extensions and pinned actions are hidden.

### Tests

Browser tests: the toolbar isn't visible and has no height; the three views
are in the header and drawn; the app menu opens from there; switching to
horizontal tabs puts them back in the toolbar and shows it, and switching
back moves them again; site info opens page info anchored to its button;
the collapsed header shows only the app menu.

### Known gaps

- With the sidebar collapsed, a download or an extension popup anchors to
  a hidden view.
- The site info icon updates on navigation and loading changes, not on
  `VisibleSecurityStateChanged()` alone (for example mixed content found
  after load).
- The permission chip in the location bar never shows; prompts are bubbles.
- No way to focus the moved controls with the keyboard as a pane (F6 still
  cycles to the hidden toolbar).

## Slice 4: Favorites

Arc's Favorites are a grid of large icons at the top of the sidebar, the
same in every space. A favorite behaves like a pinned entry: it outlives its
tab, shows dimmed while unloaded, and a click opens or activates it. Its tab
doesn't change when the window switches spaces.

### Model

Favorites are a `SpaceModel` of their own kind (`SpaceModel::Kind::kFavorites`),
owned by the profile's `SpaceList` next to its spaces but not one of them: it
has no name, isn't in `spaces()` or the switcher, and `FindSpaceOf()` never
returns it. Its pinned root holds only tabs: no folders, no splits, and
nothing goes in its Today root. Reusing `SpaceModel` gives favorites the same
observers, node IDs and `MoveToSpace()` as the spaces, so moving an entry
into Favorites and back is a move between models that keeps the node, its ID
and its live tab, as moving between spaces does.

Persisted in the sidebar file as a top-level `"favorites": [<node>...]`
(tabs only). Files without it load with no favorites, so the version stays 2.

### Tabs

`TabStripBridge` mirrors the favorites as well as the window's space:

- Strip order: live favorites first, then Pinned, then Today.
- A favorite's tab closing unloads the entry, as for Pinned. A favorite that
  navigates updates its URL and title, as a pinned entry does.
- Clicking a favorite that's live in another window moves its tab here
  (move on focus), as for any entry.
- Session restore: restored tabs first try to take over an unloaded
  favorite with the same URL (a `RestoredTabMatcher` for the favorites),
  then the space's entries.

Switching spaces leaves favorites' tabs in the window: only tabs whose node
is in the space being left move out (steps 1 and 6 of the switch). A closing
window hands them off with its other tabs.

### Adding and removing

In the sidebar page:

- Dragging a tab entry from Pinned or Today onto the grid makes it a
  favorite, at the slot it's dropped on. Its tab stays where it is. Folders
  and splits can't be dropped there.
- Dragging a tile within the grid reorders it.
- Dragging a tile into Pinned or Today moves it into the window's space. Its
  live tab then follows the rules for moving an entry between spaces
  (spaces.md, "Moving entries"): a tab live in a window showing another space
  moves to a window showing this one, or to the parking window.
- A tile's × closes its tab (it unloads); on an unloaded tile it removes
  the favorite. Same as a pinned entry.

`MoveNode()` handles these: it's the same call, with the favorites root or a
favorite as one side. The page learns the favorites through `Tree.favorites`
and the same per-node diffs (node IDs are unique across models).

### UI

The grid sits at the top of the sidebar page, above Pinned: square tiles
with the favicon, four to a row at the default width, more as the sidebar
widens. The active tile is highlighted, unloaded ones dimmed. Hovering a tile
shows its hover card. With no favorites, the grid takes no space, and a drop
zone ("Drop here to add to Favorites") appears only while an entry is dragged.

### Keyboard

Tiles join the sidebar's keyboard navigation (sidebar.md, "Keyboard
navigation") as rows before Pinned: they take the focus (`tabindex="-1"`),
the sidebar stays one tab stop, and tabbing in lands on the active entry
even when it's a favorite. Leaving the grid or the list by Tab leaves the
sidebar.

| Key on a tile | Does |
|---|---|
| ← → | Previous or next tile |
| ↑ ↓ | The tile above or below; ↓ from the grid's last row goes to the first row of Pinned (from a shorter last row's column, to the last tile first) |
| Home, End | First tile, last row of the list |
| Enter, Space | Opens or activates it |
| Delete, Backspace | As its ×: closes its tab, or removes an unloaded favorite |

↑ from the first row of Pinned goes to the last tile.

### Tests

Unit: the favorites model takes only tabs; favorites are saved and loaded
(and an older file without them loads); the bridge puts live favorites
first, unloads a favorite whose tab closes, and keeps favorites' tabs across
a space switch; restored tabs take over favorites; the page handler sends
the favorites in the tree and handles moves into, within and out of them.
Browser tests: drag a Today entry onto the grid, switch spaces, and its tab
is still the window's; click an unloaded favorite to open it; move between
tiles and into Pinned with the arrows, and Enter on a tile activates it.
Command bar unit tests: favorites are listed first and accepting one opens
it without switching spaces; add and remove move the entry; pin and unpin
aren't offered for a favorite.

### Known gaps

- Favorites have no context menu (neither has anything else yet).
- The collapsed sidebar narrows the grid to one column of tiles, as wide as
  the sidebar.
