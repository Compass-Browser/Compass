// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/window_sync/tab_strip_bridge.h"

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser_active_state_manager/browser_active_state_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/split_tab_metrics.h"
#include "chrome/browser/ui/tabs/tab_change_type.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/testing_profile_manager.h"
#include "components/split_tabs/split_tab_visual_data.h"
#include "components/tab_groups/tab_group_color.h"
#include "components/tab_groups/tab_group_id.h"
#include "components/tab_groups/tab_group_visual_data.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/restore_type.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/referrer.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/window_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace compass {
namespace {

using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;
using Type = SidebarNode::Type;

// Hosts of |parent|'s children, with splits shown as "[pane|pane]" and
// folders as "{child,child}".
std::vector<std::string> Hosts(const SidebarNode* parent) {
  std::vector<std::string> hosts;
  for (const auto& child : parent->children()) {
    if (child->type() == Type::kFolder) {
      std::string folder;
      for (const std::string& host : Hosts(child.get())) {
        folder += (folder.empty() ? "" : ",") + host;
      }
      hosts.push_back("{" + folder + "}");
    } else if (child->type() == Type::kSplit) {
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

  // Opens a link in the background from the active tab, as Cmd+click does.
  void OpenLinkInBackground(const std::string& host) {
    NavigateParams params(browser(), GURL("https://" + host + "/"),
                          ui::PAGE_TRANSITION_LINK);
    params.source_contents = tab_strip_model()->GetActiveWebContents();
    params.disposition = WindowOpenDisposition::NEW_BACKGROUND_TAB;
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

  // Adds a tab to |window| the way session restore does: with its history,
  // not loaded.
  void AddRestoredTab(BrowserWindowInterface* window, const GURL& url) {
    std::unique_ptr<content::WebContents> contents =
        content::WebContents::Create(
            content::WebContents::CreateParams(window->GetProfile()));
    std::vector<std::unique_ptr<content::NavigationEntry>> entries;
    entries.push_back(content::NavigationController::CreateNavigationEntry(
        url, content::Referrer(), std::nullopt, std::nullopt,
        ui::PAGE_TRANSITION_LINK, /*is_renderer_initiated=*/false,
        std::string(), window->GetProfile(), nullptr));
    contents->GetController().Restore(0, content::RestoreType::kRestored,
                                      &entries);
    window->GetTabStripModel()->AppendWebContents(std::move(contents),
                                                  /*foreground=*/false);
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

TEST_F(TabStripBridgeTest, ArchivesTodayTabsLeftUnseen) {
  OpenTabs({"a.test", "b.test", "c.test"});
  tab_strip_model()->ActivateTabAt(0);
  Settle();
  // a.test was shown until now.
  task_environment()->FastForwardBy(base::Hours(2));
  const base::Time switched = base::Time::Now();
  tab_strip_model()->ActivateTabAt(2);
  Settle();
  SidebarNode* a = TodayAt(0);
  EXPECT_GE(a->last_active(), switched);

  // Past the default delay for a.test and b.test, which was never shown.
  // c.test is on screen. Jumping keeps periodic tasks from running hours'
  // worth of times; the archive timer runs once it's due.
  task_environment()->AdvanceClock(kDefaultArchiveAfter + base::Hours(1));
  Settle();

  EXPECT_THAT(Hosts(space_model().today_root()), ElementsAre("c.test"));
  EXPECT_THAT(StripHosts(), ElementsAre("c.test"));
  ASSERT_EQ(space_model().archive().size(), 2u);
  EXPECT_EQ(space_model().archive()[1].tabs[0].url, GURL("https://a.test/"));

  // Restoring brings it back unloaded, at the end of Today.
  SidebarNode* restored =
      space_model().Unarchive(space_model().archive()[1].id, 1);
  ASSERT_TRUE(restored);
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("c.test", "a.test"));
  EXPECT_FALSE(bridge().GetTab(restored));
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

TEST_F(TabStripBridgeTest, StripMovesOutOfASplitSeparateIt) {
  OpenTabs({"a.test", "b.test", "c.test"});
  space_model().Split(TodayAt(0), TodayAt(1));
  Settle();
  ASSERT_TRUE(tab_strip_model()->GetTabAtIndex(0)->GetSplit());

  // Upstream ends a split whose tab moves out of it (an extension, say).
  tab_strip_model()->MoveWebContentsAt(0, 2, /*select_after_move=*/false);
  Settle();

  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("b.test", "c.test", "a.test"));
  EXPECT_THAT(StripHosts(), ElementsAre("b.test", "c.test", "a.test"));
}

TEST_F(TabStripBridgeTest, ModelSplitsSplitTheWindow) {
  OpenTabs({"a.test", "b.test", "c.test"});

  // Not side by side in the strip: the strip follows the model first.
  space_model().Split(TodayAt(0), TodayAt(2));
  Settle();

  EXPECT_THAT(StripHosts(), ElementsAre("a.test", "c.test", "b.test"));
  tabs::TabInterface* a = tab_strip_model()->GetTabAtIndex(0);
  tabs::TabInterface* c = tab_strip_model()->GetTabAtIndex(1);
  ASSERT_TRUE(a->GetSplit());
  EXPECT_EQ(a->GetSplit(), c->GetSplit());
  EXPECT_FALSE(tab_strip_model()->GetTabAtIndex(2)->GetSplit());

  // Separating in the model ends it.
  space_model().Unsplit(TodayAt(0));
  Settle();
  EXPECT_FALSE(a->GetSplit());
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("a.test", "c.test", "b.test"));
}

TEST_F(TabStripBridgeTest, UpstreamSplitsSplitTheModel) {
  OpenTabs({"a.test", "b.test", "c.test"});

  tab_strip_model()->AddToNewSplit({0, 2}, split_tabs::SplitTabVisualData(),
                                   split_tabs::SplitTabCreatedSource::
                                       kToolbarButton);
  Settle();

  ASSERT_EQ(space_model().today_root()->children().size(), 2u);
  EXPECT_EQ(TodayAt(0)->type(), Type::kSplit);
  EXPECT_EQ(StripHosts().back(), "b.test");

  // Separated upstream: the model follows, the tabs stay.
  tab_strip_model()->RemoveSplit(*tab_strip_model()->GetTabAtIndex(0)->GetSplit());
  Settle();
  EXPECT_EQ(space_model().today_root()->children().size(), 3u);
  EXPECT_EQ(tab_strip_model()->count(), 3);
}

TEST_F(TabStripBridgeTest, ClosingAPinnedPaneKeepsItsSplit) {
  OpenTabs({"a.test", "b.test", "c.test"});
  Pin(0);
  Pin(0);
  space_model().Split(space_model().pinned_root()->children()[0].get(),
                      space_model().pinned_root()->children()[1].get());
  Settle();
  ASSERT_TRUE(tab_strip_model()->GetTabAtIndex(0)->GetSplit());

  tab_strip_model()->CloseWebContentsAt(1, CLOSE_NONE);
  Settle();

  ASSERT_EQ(space_model().pinned_root()->children().size(), 1u);
  const SidebarNode* split = space_model().pinned_root()->children()[0].get();
  EXPECT_EQ(split->type(), Type::kSplit);

  // Opening a pane brings the other one, and they split again.
  bridge().OpenNode(split->children()[1].get());
  Settle();
  EXPECT_EQ(tab_strip_model()->count(), 3);
  ASSERT_TRUE(tab_strip_model()->GetTabAtIndex(0)->GetSplit());
  EXPECT_EQ(tab_strip_model()->GetTabAtIndex(0)->GetSplit(),
            tab_strip_model()->GetTabAtIndex(1)->GetSplit());
}

TEST_F(TabStripBridgeTest, LinksGatherWithTheirOpenerInAFolder) {
  OpenTabs({"a.test", "b.test"});
  tab_strip_model()->ActivateTabAt(0);

  OpenLinkInBackground("c.test");
  OpenLinkInBackground("d.test");

  // The opener's folder takes its place, named after it; links follow it in
  // the order they were opened.
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("{a.test,c.test,d.test}", "b.test"));
  EXPECT_FALSE(TodayAt(0)->title().empty());
  EXPECT_THAT(StripHosts(), ElementsAre("a.test", "c.test", "d.test",
                                        "b.test"));
  // The opener stays active.
  EXPECT_EQ(tab_strip_model()->active_index(), 0);

  // A link from a tab in the folder stays in it.
  tab_strip_model()->ActivateTabAt(2);
  OpenLinkInBackground("e.test");
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("{a.test,c.test,d.test,e.test}", "b.test"));
}

TEST_F(TabStripBridgeTest, LinksFromPinnedEntriesGatherAtTheTopOfToday) {
  OpenTabs({"a.test", "b.test"});
  SidebarNode* pinned = Pin(0);
  tab_strip_model()->ActivateTabAt(0);

  OpenLinkInBackground("c.test");
  OpenLinkInBackground("d.test");

  EXPECT_THAT(Hosts(space_model().pinned_root()), ElementsAre("a.test"));
  EXPECT_EQ(space_model().pinned_root()->children()[0].get(), pinned);
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("{c.test,d.test}", "b.test"));
}

TEST_F(TabStripBridgeTest, ForegroundAndTypedTabsDontMakeFolders) {
  OpenTabs({"a.test"});
  OpenTabAt(1, "b.test");
  NavigateParams params(browser(), GURL("https://c.test/"),
                        ui::PAGE_TRANSITION_LINK);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&params);
  Settle();

  for (const auto& entry : space_model().today_root()->children()) {
    EXPECT_NE(entry->type(), Type::kFolder);
  }
}

TEST_F(TabStripBridgeTest, TabGroupsBecomeFolders) {
  OpenTabs({"a.test", "b.test", "c.test", "d.test"});
  ASSERT_TRUE(tab_strip_model()->SupportsTabGroups());

  const tab_groups::TabGroupId group =
      tab_strip_model()->AddToNewGroup({1, 2});
  tab_strip_model()->ChangeTabGroupVisuals(
      group, tab_groups::TabGroupVisualData(
                 u"Trip", tab_groups::TabGroupColorId::kBlue,
                 /*is_collapsed=*/true));
  Settle();

  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("a.test", "{b.test,c.test}", "d.test"));
  const SidebarNode* folder = TodayAt(1);
  EXPECT_EQ(folder->title(), u"Trip");
  EXPECT_TRUE(folder->collapsed());
  // The group is gone; the tabs stay, in the tree's order.
  EXPECT_TRUE(tab_strip_model()->group_model()->ListTabGroups().empty());
  EXPECT_THAT(StripHosts(),
              ElementsAre("a.test", "b.test", "c.test", "d.test"));
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

// Sync changed the entry of a live tab (another device navigated it): the tab
// keeps its page, and only its own next navigation takes the entry back, so
// two devices don't hand the entry back and forth (sync.md, slice 3b).
TEST_F(TabStripBridgeTest, AnEntryChangedElsewhereFollowsOnlyRealChanges) {
  OpenTabs({"a.test"});
  SidebarNode* node = TodayAt(0);
  space_model().SetUrl(node, GURL("https://remote.test/"));
  space_model().SetTitle(node, u"Remote");

  // The tab updates without having changed.
  tab_strip_model()->NotifyTabChanged(tab_strip_model()->GetTabAtIndex(0),
                                      TabChangeType::kAll);
  Settle();
  EXPECT_EQ(node->url(), GURL("https://remote.test/"));
  EXPECT_EQ(node->title(), u"Remote");

  NavigateAndCommitActiveTab(GURL("https://z.test/"));
  Settle();
  EXPECT_EQ(node->url(), GURL("https://z.test/"));
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
  // The first window keeps showing a, as a placeholder.
  EXPECT_EQ(tab_strip_model()->count(), 2);
  EXPECT_EQ(bridge().GetPlaceholderNode(tab_strip_model()->GetActiveTab()), a);
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

TEST_F(TabStripBridgeTwoWindowTest, TheWindowKeepsShowingATabThatLeft) {
  // AddTab() activates each tab: a is active.
  OpenTabs({"a.test", "b.test", "c.test"});
  OpenTabInSecond("d.test");
  SidebarNode* a = TodayFor("a.test");

  second_bridge().OpenNode(a);
  Settle();

  // No switch to b or c: a placeholder for a, where a was.
  ASSERT_EQ(tab_strip_model()->count(), 3);
  EXPECT_EQ(tab_strip_model()->active_index(), 0);
  EXPECT_EQ(bridge().GetPlaceholderNode(tab_strip_model()->GetActiveTab()), a);
}

TEST_F(TabStripBridgeTwoWindowTest, MovesOutsideTheSidebarLeaveAPlaceholder) {
  OpenTabs({"a.test", "b.test"});
  OpenTabInSecond("c.test");
  SidebarNode* a = TodayFor("a.test");

  // As upstream's "Move tab to new window" or an extension would.
  std::unique_ptr<tabs::TabModel> detached =
      tab_strip_model()->DetachTabAtForInsertion(0);
  second_->GetTabStripModel()->InsertDetachedTabAt(0, std::move(detached),
                                                   AddTabTypes::ADD_ACTIVE);
  Settle();

  EXPECT_EQ(second_bridge().GetTab(a),
            second_->GetTabStripModel()->GetActiveTab());
  ASSERT_EQ(tab_strip_model()->count(), 2);
  EXPECT_EQ(tab_strip_model()->active_index(), 0);
  EXPECT_EQ(bridge().GetPlaceholderNode(tab_strip_model()->GetActiveTab()), a);
}

TEST_F(TabStripBridgeTwoWindowTest, MovingABackgroundTabLeavesNoPlaceholder) {
  OpenTabs({"a.test", "b.test"});
  OpenTabInSecond("c.test");

  second_bridge().OpenNode(TodayFor("b.test"));
  Settle();

  ASSERT_EQ(tab_strip_model()->count(), 1);
  EXPECT_EQ(bridge().GetTab(TodayFor("a.test")),
            tab_strip_model()->GetActiveTab());
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

  // Bring it back: this placeholder goes, and now the second window keeps
  // showing a, as a placeholder of its own.
  bridge().OpenNode(a);
  Settle();

  ASSERT_EQ(tab_strip_model()->count(), 1);
  EXPECT_EQ(bridge().GetTab(a), tab_strip_model()->GetActiveTab());
  EXPECT_EQ(second_->GetTabStripModel()->count(), 2);
  EXPECT_EQ(second_bridge().GetPlaceholderNode(
                second_->GetTabStripModel()->GetActiveTab()),
            a);
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

TEST_F(TabStripBridgeTwoWindowTest, FocusingTheWindowBringsTheTabBack) {
  OpenTabs({"a.test"});
  OpenTabInSecond("c.test");
  SidebarNode* a = TodayFor("a.test");
  second_bridge().OpenNode(a);
  Settle();
  ASSERT_EQ(bridge().GetPlaceholderNode(tab_strip_model()->GetActiveTab()), a);

  BrowserActiveStateManager::From(browser())->DidBecomeActive();
  Settle();

  EXPECT_EQ(bridge().GetTab(a), tab_strip_model()->GetActiveTab());
  EXPECT_EQ(tab_strip_model()->count(), 1);
  // The second window, which showed a, now shows its placeholder.
  EXPECT_EQ(second_->GetTabStripModel()->count(), 2);
}

TEST_F(TabStripBridgeTwoWindowTest, SnapshotRequestsAlwaysGetAnAnswer) {
  OpenTabs({"a.test"});
  OpenTabInSecond("c.test");
  second_bridge().OpenNode(TodayFor("a.test"));
  tabs::TabInterface* placeholder = tab_strip_model()->GetActiveTab();

  // Test views have no compositor frames, so the capture comes back empty.
  base::test::TestFuture<const std::string&> snapshot;
  bridge().GetSnapshot(placeholder, snapshot.GetCallback());
  Settle();
  EXPECT_TRUE(snapshot.IsReady());

  // A tab that isn't a placeholder has none.
  base::test::TestFuture<const std::string&> none;
  second_bridge().GetSnapshot(second_->GetTabStripModel()->GetActiveTab(),
                              none.GetCallback());
  EXPECT_EQ(none.Get(), "");
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

TEST_F(TabStripBridgeTest, ClosingTheWindowUnloadsToday) {
  OpenTabs({"a.test", "b.test"});
  AddTab(browser(), GURL("about:blank"));
  Settle();

  tab_strip_model()->CloseAllTabs();
  Settle();

  // Blank pages aren't worth keeping.
  EXPECT_THAT(Hosts(space_model().today_root()),
              ElementsAre("a.test", "b.test"));
  EXPECT_FALSE(bridge().GetTab(TodayAt(0)));
  EXPECT_FALSE(bridge().GetTab(TodayAt(1)));

  // Clicking an entry loads it again.
  EXPECT_TRUE(bridge().OpenNode(TodayAt(1)));
  EXPECT_EQ(tab_strip_model()->count(), 1);
}

TEST_F(TabStripBridgeTest, ARestoredTabTakesOverItsEntry) {
  SidebarNode* pinned = space_model().AddTab(
      space_model().pinned_root(), 0, GURL("https://p.test/"), u"P");
  SidebarNode* today = space_model().AddTab(
      space_model().today_root(), 0, GURL("https://a.test/"), u"A");

  AddRestoredTab(browser(), GURL("https://a.test/"));
  AddRestoredTab(browser(), GURL("https://p.test/"));
  // No entry for this one, and an ordinary tab never takes one over.
  AddRestoredTab(browser(), GURL("https://n.test/"));
  OpenTabAt(3, "a.test");

  EXPECT_THAT(Hosts(space_model().pinned_root()), ElementsAre("p.test"));
  EXPECT_THAT(Hosts(space_model().today_root()),
              UnorderedElementsAre("a.test", "a.test", "n.test"));
  EXPECT_TRUE(bridge().GetTab(today));
  // Pinned leads the strip.
  EXPECT_EQ(bridge().GetTab(pinned), tab_strip_model()->GetTabAtIndex(0));
}

TEST_F(TabStripBridgeTest, TabsRestoredBeforeTheLoadTakeOverTheirEntries) {
  TestingProfile* other = profile_manager()->CreateTestingProfile("other");
  ASSERT_TRUE(base::WriteFile(
      other->GetPath().AppendASCII("Compass Sidebar.json"), R"({
        "version": 1,
        "pinned": [{"type": "tab", "title": "P", "url": "https://p.test/"}],
        "today": [{"type": "tab", "title": "A", "url": "https://a.test/"}]
      })"));
  // The file loads in the background, so these come first.
  std::unique_ptr<BrowserWindowInterface> window = CreateBrowser(
      other, BrowserWindowInterface::TYPE_NORMAL, /*hosted_app=*/false);
  AddRestoredTab(window.get(), GURL("https://a.test/"));
  AddRestoredTab(window.get(), GURL("https://p.test/"));
  Settle();

  WindowFeatures* features = WindowFeatures::From(window.get());
  SpaceModel& model = features->space_model();
  EXPECT_THAT(Hosts(model.pinned_root()), ElementsAre("p.test"));
  EXPECT_THAT(Hosts(model.today_root()), ElementsAre("a.test"));
  TabStripModel* strip = window->GetTabStripModel();
  EXPECT_EQ(features->tab_strip_bridge().GetTab(
                model.pinned_root()->children()[0].get()),
            strip->GetTabAtIndex(0));
  EXPECT_EQ(features->tab_strip_bridge().GetTab(
                model.today_root()->children()[0].get()),
            strip->GetTabAtIndex(1));

  strip->CloseAllTabs();
  window.reset();
}

}  // namespace
}  // namespace compass
