// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/window_sync/tab_strip_bridge.h"

#include <memory>
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

  // Today's entry for |host|.
  SidebarNode* TodayFor(const std::string& host) {
    for (const auto& node : space_model().today_root()->children()) {
      if (node->url().host() == host) {
        return node.get();
      }
    }
    return nullptr;
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
  struct Recorder : LiveTabRegistry::Observer {
    void OnNodeLiveChanged(const SidebarNode& node, bool live) override {
      changes.push_back(std::string(node.url().host()) +
                        (live ? " live" : " unloaded"));
    }
    std::vector<std::string> changes;
  } recorder;
  OpenTabs({"a.test", "b.test"});
  SidebarNode* pinned = Pin(1);
  WindowFeatures::From(browser())->registry().AddObserver(&recorder);

  tab_strip_model()->CloseWebContentsAt(0, CLOSE_NONE);
  bridge().OpenNode(pinned);
  Settle();

  EXPECT_THAT(recorder.changes, ElementsAre("b.test unloaded", "b.test live"));
  WindowFeatures::From(browser())->registry().RemoveObserver(&recorder);
}

// Two windows of one profile share its model.
class TabStripBridgeTwoWindowTest : public TabStripBridgeTest {
 protected:
  void SetUp() override {
    TabStripBridgeTest::SetUp();
    second_ = CreateBrowser(profile(), BrowserWindowInterface::TYPE_NORMAL,
                            /*hosted_app=*/false);
  }

  void TearDown() override {
    second_->GetTabStripModel()->CloseAllTabs();
    second_.reset();
    TabStripBridgeTest::TearDown();
  }

  void OpenTabInSecond(const std::string& host) {
    AddTab(second_.get(), GURL("https://" + host + "/"));
    Settle();
  }

  TabStripBridge& second_bridge() {
    return WindowFeatures::From(second_.get())->tab_strip_bridge();
  }

  std::unique_ptr<BrowserWindowInterface> second_;
};

TEST_F(TabStripBridgeTwoWindowTest, TodayHoldsTheTabsOfBothWindows) {
  OpenTabs({"a.test"});
  OpenTabInSecond("b.test");

  EXPECT_EQ(&WindowFeatures::From(second_.get())->space_model(),
            &space_model());
  // The second window's tab follows no tab of its own window, so it leads.
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("b.test", "a.test"));
  // Each bridge sees only its own window's tabs.
  EXPECT_TRUE(bridge().GetTab(TodayFor("a.test")));
  EXPECT_FALSE(bridge().GetTab(TodayFor("b.test")));
  EXPECT_TRUE(second_bridge().GetTab(TodayFor("b.test")));
}

TEST_F(TabStripBridgeTwoWindowTest, OpeningANodeMovesItsTabHere) {
  OpenTabs({"a.test", "b.test"});
  OpenTabInSecond("c.test");
  SidebarNode* a = TodayFor("a.test");
  content::WebContents* contents = tab_strip_model()->GetWebContentsAt(0);

  tabs::TabInterface* tab = second_bridge().OpenNode(a);
  Settle();

  // The same tab and page, now in the second window, with the same node.
  ASSERT_TRUE(tab);
  EXPECT_EQ(tab->GetContents(), contents);
  EXPECT_EQ(second_->GetTabStripModel()->count(), 2);
  EXPECT_EQ(second_->GetTabStripModel()->GetActiveTab(), tab);
  EXPECT_EQ(tab_strip_model()->count(), 1);
  // Moving doesn't change the tree.
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("c.test", "a.test", "b.test"));
  EXPECT_FALSE(bridge().GetTab(a));
  EXPECT_EQ(second_bridge().GetTab(a), tab);
}

TEST_F(TabStripBridgeTwoWindowTest, PinnedIsSharedAndUnloadsEverywhere) {
  OpenTabs({"a.test"});
  OpenTabInSecond("b.test");
  SidebarNode* pinned = TodayFor("a.test");
  space_model().Move(pinned, space_model().pinned_root(), 0);
  Settle();

  tab_strip_model()->CloseWebContentsAt(0, CLOSE_NONE);

  EXPECT_THAT(Hosts(space_model().pinned_root()), ElementsAre("a.test"));
  EXPECT_FALSE(WindowFeatures::From(second_.get())->registry().GetTab(pinned));

  // Reopening it from the second window opens it there.
  EXPECT_TRUE(second_bridge().OpenNode(pinned));
}

TEST_F(TabStripBridgeTwoWindowTest, MovingTheActiveTabFallsBackToTheLastUsed) {
  // AddTab() activates each tab: a is active, b was used before it.
  OpenTabs({"a.test", "b.test", "c.test"});
  tab_strip_model()->ActivateTabAt(1);  // b
  tab_strip_model()->ActivateTabAt(0);  // a
  OpenTabInSecond("d.test");

  second_bridge().OpenNode(TodayFor("a.test"));
  Settle();

  EXPECT_EQ(tab_strip_model()->GetActiveWebContents()->GetLastCommittedURL(),
            GURL("https://b.test/"));
}

TEST_F(TabStripBridgeTwoWindowTest, MovingTheLastTabLeavesAPlaceholder) {
  OpenTabs({"a.test"});
  OpenTabInSecond("c.test");
  SidebarNode* a = TodayFor("a.test");

  second_bridge().OpenNode(a);
  Settle();

  // The first window stays open, showing a placeholder for a.
  ASSERT_EQ(tab_strip_model()->count(), 1);
  tabs::TabInterface* placeholder = tab_strip_model()->GetActiveTab();
  EXPECT_EQ(bridge().GetPlaceholderNode(placeholder), a);
  EXPECT_EQ(bridge().GetShownNode(placeholder), a);
  EXPECT_FALSE(bridge().GetNode(placeholder));
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("c.test", "a.test"));

  // Bring it back: the placeholder goes, and the second window falls back.
  bridge().OpenNode(a);
  Settle();

  ASSERT_EQ(tab_strip_model()->count(), 1);
  EXPECT_EQ(bridge().GetTab(a), tab_strip_model()->GetActiveTab());
  EXPECT_EQ(second_->GetTabStripModel()->count(), 1);
  EXPECT_EQ(
      second_bridge().GetNode(second_->GetTabStripModel()->GetActiveTab()),
      TodayFor("c.test"));
}

TEST_F(TabStripBridgeTwoWindowTest, PlaceholderClosesWhenItsNodeIsRemoved) {
  OpenTabs({"a.test"});
  OpenTabInSecond("c.test");
  second_bridge().OpenNode(TodayFor("a.test"));
  Settle();
  ASSERT_TRUE(bridge().GetPlaceholderNode(tab_strip_model()->GetActiveTab()));
  // A background tab keeps the window open once the placeholder goes.
  NavigateParams params(browser(), GURL("https://e.test/"),
                        ui::PAGE_TRANSITION_TYPED);
  params.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
  Navigate(&params);
  CommitPendingLoad(&params.navigated_or_inserted_contents->GetController());
  Settle();
  ASSERT_EQ(tab_strip_model()->count(), 2);

  space_model().Remove(TodayFor("a.test"));
  Settle();

  ASSERT_EQ(tab_strip_model()->count(), 1);
  EXPECT_EQ(tab_strip_model()->GetActiveWebContents()->GetLastCommittedURL(),
            GURL("https://e.test/"));
}

TEST_F(TabStripBridgeTwoWindowTest, ClosingAWindowHandsItsTabsOver) {
  OpenTabs({"a.test"});
  OpenTabInSecond("b.test");
  OpenTabInSecond("c.test");
  tabs::TabInterface* b_tab = second_->GetTabStripModel()->GetTabAtIndex(1);

  EXPECT_TRUE(HandOffTabsBeforeClose(second_.get()));
  Settle();

  EXPECT_TRUE(second_->GetTabStripModel()->empty());
  EXPECT_EQ(tab_strip_model()->count(), 3);
  // The same tab, not a reload, and the first window keeps its active tab.
  EXPECT_EQ(bridge().GetTab(TodayFor("b.test")), b_tab);
  EXPECT_EQ(tab_strip_model()->GetActiveWebContents()->GetLastCommittedURL(),
            GURL("https://a.test/"));
  EXPECT_EQ(space_model().today_root()->children().size(), 3u);
}

TEST_F(TabStripBridgeTest, TheLastWindowKeepsItsTabs) {
  OpenTabs({"a.test"});

  EXPECT_FALSE(HandOffTabsBeforeClose(browser()));
  EXPECT_EQ(tab_strip_model()->count(), 1);
}

}  // namespace
}  // namespace strata
