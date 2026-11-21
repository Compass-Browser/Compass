// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/tab_switcher/recent_entries.h"

#include <vector>

#include "base/time/time.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_model.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {

namespace {

std::vector<const SidebarNode*> Entries(const std::vector<RecentEntry>& v) {
  std::vector<const SidebarNode*> out;
  for (const RecentEntry& e : v) {
    out.push_back(e.entry);
  }
  return out;
}

class RecentEntriesTest : public testing::Test {
 protected:
  SidebarNode* Tab(SpaceModel& model, SidebarNode* parent, int hours_ago) {
    SidebarNode* tab = model.AddTab(parent, parent->children().size(),
                                    GURL("https://example.com/"), u"Tab");
    if (hours_ago >= 0) {
      model.SetLastActive(tab, now_ - base::Hours(hours_ago));
    }
    return tab;
  }

  const base::Time now_ = base::Time::Now();
  SpaceModel space_;
  SpaceModel favorites_;
};

TEST_F(RecentEntriesTest, ShownFirstThenNewest) {
  SidebarNode* a = Tab(space_, space_.today_root(), 3);
  SidebarNode* b = Tab(space_, space_.pinned_root(), 1);
  SidebarNode* fav = Tab(favorites_, favorites_.pinned_root(), 2);
  SidebarNode* shown = Tab(space_, space_.today_root(), 5);

  EXPECT_EQ(Entries(GetRecentEntries(space_, favorites_, shown)),
            (std::vector<const SidebarNode*>{shown, b, fav, a}));
  std::vector<RecentEntry> recent =
      GetRecentEntries(space_, favorites_, shown);
  EXPECT_TRUE(recent[2].favorite);
  EXPECT_FALSE(recent[1].favorite);
}

TEST_F(RecentEntriesTest, NeverShownLeftOut) {
  SidebarNode* a = Tab(space_, space_.today_root(), 1);
  Tab(space_, space_.today_root(), -1);
  EXPECT_EQ(Entries(GetRecentEntries(space_, favorites_, nullptr)),
            (std::vector<const SidebarNode*>{a}));
}

TEST_F(RecentEntriesTest, SplitIsOneEntry) {
  SidebarNode* other = Tab(space_, space_.today_root(), 2);
  SidebarNode* split = space_.AddSplit(space_.today_root(), 1);
  SidebarNode* left = Tab(space_, split, 4);
  SidebarNode* right = Tab(space_, split, 1);
  SidebarNode* shown = Tab(space_, space_.today_root(), 0);

  std::vector<RecentEntry> recent =
      GetRecentEntries(space_, favorites_, shown);
  ASSERT_EQ(recent.size(), 3u);
  EXPECT_EQ(recent[1].entry, split);
  // It opens its newest pane.
  EXPECT_EQ(recent[1].tab, right);
  EXPECT_EQ(recent[2].entry, other);

  // Shown, the split comes first and keeps the shown pane.
  recent = GetRecentEntries(space_, favorites_, left);
  EXPECT_EQ(recent[0].entry, split);
  EXPECT_EQ(recent[0].tab, left);
}

}  // namespace

}  // namespace compass
