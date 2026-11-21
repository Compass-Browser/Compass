// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_RECENT_ENTRIES_H_
#define COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_RECENT_ENTRIES_H_

#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/time/time.h"

namespace compass {

class SidebarNode;
class SpaceModel;

// An entry Ctrl+Tab can switch to: a tab, or a split as one entry.
struct RecentEntry {
  // The tab or split node.
  raw_ptr<SidebarNode> entry;
  // What switching to it opens: the tab, or the split's most recent pane.
  raw_ptr<SidebarNode> tab;
  base::Time last_active;
  bool favorite = false;
};

// The window's entries in recently used order (docs/design/sidebar.md,
// "Recent tabs"): its space's Pinned and Today, and the favorites, those
// that were ever shown (SidebarNode::last_active()), newest first, with
// |shown| (the window's entry, a tab or a split's pane) first whatever its
// time. Folders are walked into; a split is one entry.
std::vector<RecentEntry> GetRecentEntries(SpaceModel& space,
                                          SpaceModel& favorites,
                                          const SidebarNode* shown);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_TAB_SWITCHER_RECENT_ENTRIES_H_
