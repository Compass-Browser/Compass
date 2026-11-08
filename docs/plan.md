# Plan

Work decided but not started, in no particular order. Each item gets a
design doc section (or its own doc) when it starts; the links point where
it belongs.

## Sidebar

- **Middle click closes a tab.** A middle click on a sidebar entry (a tab,
  or a split's pane) does what its ×/− button does, by the same rules
  (`onCloseClick_()`, `resources/sidebar/node.ts`): an open tab closes (a
  pinned one unloads and goes back to its pinned URL), and an unloaded
  pinned entry is removed only on a second middle click while armed.
  Folders and spaces ignore it. [sidebar.md](design/sidebar.md).
- **Dragging a tab out of the windows makes it a little window.** Dropping
  a sidebar entry outside every Compass window opens that tab, live, in a
  little window at the drop point (it leaves the sidebar, as when a little
  window's tab is promoted the other way). Replaces the "Not taken over
  yet" drag into a new window. [sidebar.md](design/sidebar.md),
  [little-window.md](design/little-window.md).

## Little windows

- **The address can be edited.** Upstream's popup location bar is
  read-only; a little window's becomes editable, and a new empty little
  window starts with it focused. [little-window.md](design/little-window.md).
- **Space icons** in the "Open in space" picker, and in the command bar's
  space results. [spaces.md](design/spaces.md) "Not done",
  [command-bar.md](design/command-bar.md).

## Split view

- **Up to 4 panes.** `kMaxSplitPanes` goes from 2 to 4, which means
  extending upstream's `MultiContentsView` (two panes) and
  `SplitTabData` to more panes and layouts. The sidebar's Split node is
  already a list of tabs capped by the constant
  (`browser/sidebar/space_model.h`). ARCHITECTURE.md §9.
