// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <vector>

#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace strata {
namespace {

using ::testing::ElementsAre;

// Hosts of the pages in |strip|, left to right.
std::vector<std::string> Hosts(TabStripModel* strip) {
  std::vector<std::string> hosts;
  for (int i = 0; i < strip->count(); ++i) {
    hosts.push_back(
        std::string(strip->GetWebContentsAt(i)->GetLastCommittedURL().host()));
  }
  return hosts;
}

// Hosts of |parent|'s children.
std::vector<std::string> Hosts(const SidebarNode* parent) {
  std::vector<std::string> hosts;
  for (const auto& child : parent->children()) {
    hosts.push_back(std::string(child->url().host()));
  }
  return hosts;
}

class SpaceSwitchTest : public BrowserWithTestWindowTest {
 protected:
  // Mock time lets tests step past Browser's coalesced UI updates.
  SpaceSwitchTest()
      : BrowserWithTestWindowTest(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  void SetUp() override {
    BrowserWithTestWindowTest::SetUp();
    // Test windows can't be created the production way.
    service().SetParkingWindowFactoryForTesting(
        base::BindLambdaForTesting([this](Profile* profile) {
          parking_ = CreateBrowser(profile, BrowserWindowInterface::TYPE_POPUP,
                                   /*hosted_app=*/false);
          return parking_.get();
        }));
    home_ = &list().active_space();
    work_ = list().AddSpace(1, u"Work");
  }

  void TearDown() override {
    for (auto* window : {second_.get(), parking_.get()}) {
      if (window) {
        window->GetTabStripModel()->CloseAllTabs();
      }
    }
    second_.reset();
    parking_.reset();
    BrowserWithTestWindowTest::TearDown();
  }

  // Opens tabs so the strip reads |hosts|, left to right.
  void OpenTabs(BrowserWindowInterface* window,
                const std::vector<std::string>& hosts) {
    // AddTab() inserts at the front and activates.
    for (auto it = hosts.rbegin(); it != hosts.rend(); ++it) {
      AddTab(window, GURL("https://" + *it + "/"));
    }
    Settle();
  }

  // Runs pending tasks and Browser's coalesced UI updates.
  void Settle() { task_environment()->FastForwardBy(base::Seconds(1)); }

  void SwitchTo(BrowserWindowInterface* window, SpaceModel& space) {
    WindowFeatures::From(window)->SwitchToSpace(space);
    Settle();
  }

  WindowFeatures& features() { return *WindowFeatures::From(browser()); }
  SpaceService& service() { return features().service(); }
  SpaceList& list() { return service().space_list(); }
  TabStripModel* strip() { return browser()->GetTabStripModel(); }

  raw_ptr<SpaceModel> home_ = nullptr;
  raw_ptr<SpaceModel> work_ = nullptr;
  std::unique_ptr<BrowserWindowInterface> second_;
  std::unique_ptr<BrowserWindowInterface> parking_;
};

TEST_F(SpaceSwitchTest, ParksTheTabsAndBringsThemBack) {
  OpenTabs(browser(), {"a.test", "b.test"});
  strip()->ActivateTabAt(1);
  content::WebContents* b = strip()->GetWebContentsAt(1);

  SwitchTo(browser(), *work_);

  EXPECT_EQ(&features().space_model(), work_);
  EXPECT_EQ(&list().active_space(), work_);
  // The new space had nothing to show, so it got a new tab.
  EXPECT_EQ(strip()->count(), 1);
  EXPECT_EQ(work_->today_root()->children().size(), 1u);
  // Home's tabs are alive in the parking window, with their entries.
  ASSERT_TRUE(parking_);
  EXPECT_THAT(Hosts(parking_->GetTabStripModel()),
              ElementsAre("a.test", "b.test"));
  EXPECT_THAT(Hosts(home_->today_root()), ElementsAre("a.test", "b.test"));
  EXPECT_TRUE(features().registry().GetTab(
      home_->today_root()->children()[0].get()));

  SwitchTo(browser(), *home_);

  // The same tabs, on the one the space showed last. The blank tab of Work
  // closed with its entry.
  EXPECT_THAT(Hosts(strip()), ElementsAre("a.test", "b.test"));
  EXPECT_EQ(strip()->GetActiveWebContents(), b);
  EXPECT_TRUE(work_->today_root()->children().empty());
  EXPECT_TRUE(parking_->GetTabStripModel()->empty());
}

TEST_F(SpaceSwitchTest, OpensTheFirstEntryOfASpaceWithNothingLive) {
  OpenTabs(browser(), {"a.test"});
  work_->AddTab(work_->pinned_root(), 0, GURL("https://p.test/"), u"P");

  SwitchTo(browser(), *work_);

  ASSERT_EQ(strip()->count(), 1);
  EXPECT_EQ(features().tab_strip_bridge().GetNode(strip()->GetActiveTab()),
            work_->pinned_root()->children()[0].get());
  EXPECT_EQ(work_->today_root()->children().size(), 0u);
}

TEST_F(SpaceSwitchTest, AnotherWindowOnTheSpaceTakesTheTabs) {
  OpenTabs(browser(), {"a.test"});
  second_ = CreateBrowser(profile(), BrowserWindowInterface::TYPE_NORMAL,
                          /*hosted_app=*/false);
  OpenTabs(second_.get(), {"b.test"});

  SwitchTo(browser(), *work_);

  EXPECT_THAT(Hosts(second_->GetTabStripModel()),
              ElementsAre("b.test", "a.test"));
  EXPECT_FALSE(parking_);
}

TEST_F(SpaceSwitchTest, ClosingAWindowHandsTabsOnlyToItsSpace) {
  OpenTabs(browser(), {"a.test"});
  second_ = CreateBrowser(profile(), BrowserWindowInterface::TYPE_NORMAL,
                          /*hosted_app=*/false);
  OpenTabs(second_.get(), {"b.test"});
  // The second window's Home tab comes to this window, which shows Home.
  SwitchTo(second_.get(), *work_);
  ASSERT_THAT(Hosts(strip()), ElementsAre("b.test", "a.test"));

  // No other window shows Home, so closing this one hands nothing off.
  EXPECT_FALSE(HandOffTabsBeforeClose(browser()));
  EXPECT_EQ(strip()->count(), 2);
}

TEST_F(SpaceSwitchTest, NewWindowsOpenInTheActiveSpace) {
  SwitchTo(browser(), *work_);

  second_ = CreateBrowser(profile(), BrowserWindowInterface::TYPE_NORMAL,
                          /*hosted_app=*/false);

  EXPECT_EQ(&WindowFeatures::From(second_.get())->space_model(), work_);
}

}  // namespace
}  // namespace strata
