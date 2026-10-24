# Command bar and toolbar-free window

Status: slice 1 done (command bar); slice 2 (sidebar header) next. Builds on ARCHITECTURE.md §4 and §11 and on
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
- **Entries**: tab nodes of the profile's spaces whose title or URL
  contains every word of the query, the window's space first, at most five.
  Shown as "Switch to". Choosing one in another space switches the window
  there first.
- **Spaces**: whose name contains the query. Choosing one switches to it.
- **Commands**: a fixed list (new space, pin or unpin the current entry,
  copy URL, reload, history, downloads, extensions, settings), matched by
  name. Commands that need an entry are left out when there's none.

Order: the default omnibox match, entries, the other omnibox matches,
spaces, commands. With an empty query in new-entry mode the list is the
window's Today entries, most recent first, then the commands.

### Accepting

- An omnibox match navigates to `match.destination_url` with the match's
  transition (plus `FROM_ADDRESS_BAR`): in a new foreground tab (a new
  Today entry) in new-entry mode, in the active tab otherwise. The typed
  text and match go to the profile's `ShortcutsBackend`, so the shortcuts
  provider learns from it as it does for the omnibox; history learns from
  the navigation itself. No `OmniboxLog` is reported yet.
- Cmd+Enter on an omnibox match opens it as a new entry in either mode.
- An entry is activated (opened if unloaded) in this window.

The bubble closes after any accept, and when the window loses focus.

### Tests

`CommandBarPageHandlerTest` (unit): result order for empty and typed
queries, other spaces' entries, stable IDs within a query, and accepting
each kind of result, including a stale ID. `CommandBarBrowserTest`: Cmd+T
then a URL and Enter makes a Today entry, Cmd+L starts from the page's URL
and loads in place, and opening again starts over. Esc is handled by the
bubble natively, which a synthetic key event in the page can't reach, so
it's checked by hand.

### Known gaps

- The hook sits in
  `BrowserCommandController::ExecuteCommandWithDispositionAndContext()`,
  which menus and accelerators go through whether or not upstream runs
  commands as browser actions. Anything that invokes the
  New Tab action item directly (upstream's app menu redesign, if enabled)
  still opens a plain new tab.
- Hovering selects a row, so a pointer resting where the bubble appears
  selects that row instead of the default suggestion.
- No `OmniboxLog` is reported (see Accepting).
