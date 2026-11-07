// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_WINDOW_SYNC_UNDO_CLOSE_H_
#define COMPASS_BROWSER_WINDOW_SYNC_UNDO_CLOSE_H_

class BrowserWindowInterface;

namespace compass {

class ClosedEntries;
class LiveTabRegistry;
class SidebarNode;

// Cmd+Z and Cmd+Shift+Z for the sidebar entries the user closed, from a
// profile's ClosedEntries. See docs/design/sidebar.md, "Undo closing".

// Whether |browser|'s profile has a close to undo, or to redo.
bool CanUndoClose(BrowserWindowInterface* browser);
bool CanRedoClose(BrowserWindowInterface* browser);

// Puts back what the user closed last, in |browser|'s window: the window
// switches to the entries' space, they come back where they were, unloaded,
// and the tab the window showed (else the first that was open) opens with
// its history and is shown. Returns false if there was nothing to undo.
bool UndoClose(BrowserWindowInterface* browser);

// Closes again what UndoClose() last put back. Returns false if there was
// nothing to redo.
bool RedoClose(BrowserWindowInterface* browser);

// Closes the live tab of |tab_node|, in whichever window has it, as its ×
// does: that window's strip closes it, and its bridge records the close.
// Returns false if no window with a bridge has it (it's unloaded, or
// parked).
bool CloseLiveTab(LiveTabRegistry& registry, const SidebarNode& tab_node);

// Tells |closed| that the live tabs under |node| are about to close for the
// step just recorded.
void ExpectLiveTabsToClose(LiveTabRegistry& registry,
                           const SidebarNode& node,
                           ClosedEntries& closed);

}  // namespace compass

#endif  // COMPASS_BROWSER_WINDOW_SYNC_UNDO_CLOSE_H_
