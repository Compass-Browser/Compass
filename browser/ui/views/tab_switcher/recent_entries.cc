// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/tab_switcher/recent_entries.h"

#include <algorithm>

#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"

namespace compass {

namespace {

void Collect(SidebarNode& node,
             bool favorite,
             const SidebarNode* shown,
             std::vector<RecentEntry>& out) {
  switch (node.type()) {
    case SidebarNode::Type::kTab:
      out.push_back({&node, &node, node.last_active(), favorite});
      return;
    case SidebarNode::Type::kSplit: {
      // One entry: its time is its newest pane's, and it opens that pane
      // (or the one the window shows).
      RecentEntry split{&node, nullptr, base::Time(), favorite};
      for (const auto& pane : node.children()) {
        if (pane->type() != SidebarNode::Type::kTab) {
          continue;
        }
        if (!split.tab || pane.get() == shown ||
            (split.tab != shown && pane->last_active() > split.last_active)) {
          split.tab = pane.get();
        }
        split.last_active = std::max(split.last_active, pane->last_active());
      }
      if (split.tab) {
        out.push_back(split);
      }
      return;
    }
    default:
      for (const auto& child : node.children()) {
        Collect(*child, favorite, shown, out);
      }
  }
}

}  // namespace

std::vector<RecentEntry> GetRecentEntries(SpaceModel& space,
                                          SpaceModel& favorites,
                                          const SidebarNode* shown) {
  std::vector<RecentEntry> all;
  Collect(*favorites.pinned_root(), /*favorite=*/true, shown, all);
  Collect(*space.pinned_root(), /*favorite=*/false, shown, all);
  Collect(*space.today_root(), /*favorite=*/false, shown, all);

  auto is_shown = [shown](const RecentEntry& e) {
    return shown && (e.entry == shown || e.entry->Contains(shown));
  };
  std::vector<RecentEntry> recent;
  for (const RecentEntry& e : all) {
    if (is_shown(e) || !e.last_active.is_null()) {
      recent.push_back(e);
    }
  }
  std::ranges::stable_sort(recent, [&](const RecentEntry& a,
                                       const RecentEntry& b) {
    const bool a_shown = is_shown(a);
    if (a_shown != is_shown(b)) {
      return a_shown;
    }
    return a.last_active > b.last_active;
  });
  return recent;
}

}  // namespace compass
