# Plan

Work decided but not started, in no particular order. Each item gets a
design doc section (or its own doc) when it starts; the links point where
it belongs.

## Split view

- **Up to 4 panes.** `kMaxSplitPanes` goes from 2 to 4, which means
  extending upstream's `MultiContentsView` (two panes) and
  `SplitTabData` to more panes and layouts. The sidebar's Split node is
  already a list of tabs capped by the constant
  (`browser/sidebar/space_model.h`). ARCHITECTURE.md §9.
