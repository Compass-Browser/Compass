// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/tab_strip_bridge.h"

#include <string>
#include <vector>

#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace strata {
namespace {

using ::testing::ElementsAre;
using Type = SidebarNode::Type;

// Hosts of |parent|'s children, with splits shown as "[pane|pane]".
std::vector<std::string> Hosts(const SidebarNode* parent) {
  std::vector<std::string> hosts;
  for (const auto& child : parent->children()) {
    if (child->type() == Type::kSplit) {
      std::string split;
      for (const auto& pane : child->children()) {
        split += (split.empty() ? "" : "|") + std::string(pane->url().host());
      }
      hosts.push_back("[" + split + "]");
    } else {
      hosts.push_back(std::string(child->url().host()));
    }
  }
  return hosts;
}

class TabStripBridgeTest : public BrowserWithTestWindowTest {
 protected:
  // Mock time lets tests step past Browser's coalesced UI updates, which
  // carry committed URLs.
  TabStripBridgeTest()
      : BrowserWithTestWindowTest(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  // Opens tabs so the strip reads |hosts|, left to right.
  void OpenTabs(const std::vector<std::string>& hosts) {
    // AddTab() inserts at the front.
    for (auto it = hosts.rbegin(); it != hosts.rend(); ++it) {
      AddTab(browser(), GURL("https://" + *it + "/"));
    }
    Settle();
  }

  // Opens a tab at strip |index|.
  void OpenTabAt(int index, const std::string& host) {
    NavigateParams params(browser(), GURL("https://" + host + "/"),
                          ui::PAGE_TRANSITION_TYPED);
    params.tabstrip_index = index;
    params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
    Navigate(&params);
    CommitPendingLoad(&params.navigated_or_inserted_contents->GetController());
    Settle();
  }

  // Runs pending tasks and Browser's coalesced UI updates.
  void Settle() { task_environment()->FastForwardBy(base::Seconds(1)); }

  std::vector<std::string> StripHosts() {
    std::vector<std::string> hosts;
    for (int i = 0; i < tab_strip_model()->count(); ++i) {
      hosts.push_back(std::string(tab_strip_model()
                                      ->GetWebContentsAt(i)
                                      ->GetLastCommittedURL()
                                      .host()));
    }
    return hosts;
  }

  // Pins Today's entry at |index| at the end of the pinned section.
  SidebarNode* Pin(size_t index) {
    SidebarNode* node = TodayAt(index);
    space_model().Move(node, space_model().pinned_root(),
                       space_model().pinned_root()->children().size());
    Settle();
    return node;
  }

  SidebarNode* TodayAt(size_t index) {
    return space_model().today_root()->children()[index].get();
  }

  TabStripModel* tab_strip_model() { return browser()->GetTabStripModel(); }
  SpaceModel& space_model() {
    return WindowFeatures::From(browser())->space_model();
  }
  TabStripBridge& bridge() {
    return WindowFeatures::From(browser())->tab_strip_bridge();
  }
};

TEST_F(TabStripBridgeTest, MirrorsOpenTabsIntoToday) {
  OpenTabs({"a.test", "b.test", "c.test"});

  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("a.test", "b.test", "c.test"));
  EXPECT_TRUE(space_model().pinned_root()->children().empty());
  EXPECT_EQ(bridge().GetTab(TodayAt(1)), tab_strip_model()->GetTabAtIndex(1));
  EXPECT_EQ(bridge().GetNode(tab_strip_model()->GetTabAtIndex(2)), TodayAt(2));
}

TEST_F(TabStripBridgeTest, RemovesTheNodeWhenItsTabCloses) {
  OpenTabs({"a.test", "b.test"});

  tab_strip_model()->CloseWebContentsAt(0, CLOSE_NONE);

  EXPECT_THAT(Hosts(space_model().today_root()), ElementsAre("b.test"));
}

TEST_F(TabStripBridgeTest, MirrorsStripMoves) {
  OpenTabs({"a.test", "b.test", "c.test"});

  tab_strip_model()->MoveWebContentsAt(0, 2, /*select_after_move=*/false);
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("b.test", "c.test", "a.test"));

  tab_strip_model()->MoveWebContentsAt(2, 1, /*select_after_move=*/false);
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("b.test", "a.test", "c.test"));
}

TEST_F(TabStripBridgeTest, ReordersTheStripWhenTheModelMoves) {
  OpenTabs({"a.test", "b.test", "c.test"});

  space_model().Move(TodayAt(2), space_model().today_root(), 0);
  Settle();

  EXPECT_THAT(StripHosts(), ElementsAre("c.test", "a.test", "b.test"));
}

TEST_F(TabStripBridgeTest, ClosesTheTabWhenItsNodeIsRemoved) {
  OpenTabs({"a.test", "b.test"});

  space_model().Remove(TodayAt(0));
  Settle();

  EXPECT_THAT(StripHosts(), ElementsAre("b.test"));
  EXPECT_THAT(Hosts(space_model().today_root()), ElementsAre("b.test"));
}

TEST_F(TabStripBridgeTest, ClosingAPaneDissolvesTheSplit) {
  OpenTabs({"a.test", "b.test", "c.test"});
  space_model().Split(TodayAt(0), TodayAt(2));
  Settle();
  ASSERT_THAT(Hosts(space_model().today_root()),
              ElementsAre("[a.test|c.test]", "b.test"));
  ASSERT_THAT(StripHosts(), ElementsAre("a.test", "c.test", "b.test"));

  tab_strip_model()->CloseWebContentsAt(0, CLOSE_NONE);

  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("c.test", "b.test"));
}

TEST_F(TabStripBridgeTest, UndoesStripMovesOutOfASplit) {
  OpenTabs({"a.test", "b.test", "c.test"});
  space_model().Split(TodayAt(0), TodayAt(1));
  Settle();

  tab_strip_model()->MoveWebContentsAt(0, 2, /*select_after_move=*/false);
  Settle();

  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("[a.test|b.test]", "c.test"));
  EXPECT_THAT(StripHosts(), ElementsAre("a.test", "b.test", "c.test"));
}

TEST_F(TabStripBridgeTest, NewTabFollowsTheTabBeforeIt) {
  OpenTabs({"a.test", "b.test"});
  space_model().Split(TodayAt(0), TodayAt(1));
  Settle();

  // Inserted after the second pane: the new entry follows the whole split.
  OpenTabAt(2, "c.test");

  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("[a.test|b.test]", "c.test"));
}

TEST_F(TabStripBridgeTest, FollowsNavigation) {
  OpenTabs({"a.test"});

  NavigateAndCommitActiveTab(GURL("https://z.test/"));
  Settle();

  EXPECT_THAT(Hosts(space_model().today_root()), ElementsAre("z.test"));
}

TEST_F(TabStripBridgeTest, PinnedTabsLeadTheStrip) {
  OpenTabs({"a.test", "b.test"});

  Pin(1);

  EXPECT_THAT(Hosts(space_model().pinned_root()), ElementsAre("b.test"));
  EXPECT_THAT(StripHosts(), ElementsAre("b.test", "a.test"));
}

TEST_F(TabStripBridgeTest, PinnedNodeStaysWhenItsTabCloses) {
  OpenTabs({"a.test", "b.test"});
  SidebarNode* pinned = Pin(1);

  tab_strip_model()->CloseWebContentsAt(0, CLOSE_NONE);

  EXPECT_THAT(Hosts(space_model().pinned_root()), ElementsAre("b.test"));
  EXPECT_EQ(bridge().GetTab(pinned), nullptr);
  EXPECT_THAT(StripHosts(), ElementsAre("a.test"));
}

TEST_F(TabStripBridgeTest, OpenNodeReopensAnUnloadedPinnedNode) {
  OpenTabs({"a.test", "b.test"});
  SidebarNode* pinned = Pin(1);
  tab_strip_model()->CloseWebContentsAt(0, CLOSE_NONE);

  tabs::TabInterface* tab = bridge().OpenNode(pinned);
  ASSERT_TRUE(tab);
  CommitPendingLoad(&tab->GetContents()->GetController());
  Settle();

  EXPECT_EQ(bridge().GetTab(pinned), tab);
  EXPECT_EQ(tab_strip_model()->GetActiveTab(), tab);
  EXPECT_THAT(StripHosts(), ElementsAre("b.test", "a.test"));
  // Adopted, not mirrored into Today.
  EXPECT_THAT(Hosts(space_model().today_root()), ElementsAre("a.test"));
}

TEST_F(TabStripBridgeTest, NewTabsLandAfterThePinnedBlock) {
  OpenTabs({"a.test", "b.test"});
  Pin(1);

  OpenTabAt(0, "c.test");

  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("c.test", "a.test"));
  EXPECT_THAT(StripHosts(), ElementsAre("b.test", "c.test", "a.test"));
}

TEST_F(TabStripBridgeTest, ReportsLiveChanges) {
  struct Recorder : TabStripBridge::Observer {
    void OnNodeLiveChanged(const SidebarNode& node, bool live) override {
      changes.push_back(std::string(node.url().host()) +
                        (live ? " live" : " unloaded"));
    }
    std::vector<std::string> changes;
  } recorder;
  bridge().AddObserver(&recorder);
  OpenTabs({"a.test", "b.test"});
  SidebarNode* pinned = Pin(1);

  tab_strip_model()->CloseWebContentsAt(0, CLOSE_NONE);
  bridge().OpenNode(pinned);
  Settle();

  EXPECT_THAT(recorder.changes, ElementsAre("b.test unloaded", "b.test live"));
  bridge().RemoveObserver(&recorder);
}

}  // namespace
}  // namespace strata
