// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <vector>

#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "chrome/test/base/testing_profile.h"
#include "chrome/test/base/testing_profile_manager.h"
#include "content/public/browser/web_contents.h"
#include "strata/browser/sidebar/space_list.h"
#include "strata/browser/sidebar/space_model.h"
#include "strata/browser/window_sync/space_directory.h"
#include "strata/browser/window_sync/space_service.h"
#include "strata/browser/window_sync/window_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/gfx/geometry/rect.h"
#include "url/gurl.h"

namespace strata {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

// Hosts of the pages in |strip|, left to right.
std::vector<std::string> Hosts(TabStripModel* strip) {
  std::vector<std::string> hosts;
  for (int i = 0; i < strip->count(); ++i) {
    hosts.push_back(
        std::string(strip->GetWebContentsAt(i)->GetLastCommittedURL().host()));
  }
  return hosts;
}

// Switching to spaces of another profile (docs/design/spaces.md, slice 3).
class SpaceSwapTest : public BrowserWithTestWindowTest {
 protected:
  SpaceSwapTest()
      : BrowserWithTestWindowTest(
            base::test::TaskEnvironment::TimeSource::MOCK_TIME) {}

  void SetUp() override {
    BrowserWithTestWindowTest::SetUp();
    other_profile_ = profile_manager()->CreateTestingProfile("other");
    // Test windows can't be created the production way.
    for (Profile* p : std::vector<Profile*>{profile(), other_profile_.get()}) {
      SpaceService::Get(p)->SetParkingWindowFactoryForTesting(
          base::BindLambdaForTesting([this](Profile* profile) {
            windows_.push_back(CreateBrowser(
                profile, BrowserWindowInterface::TYPE_POPUP,
                /*hosted_app=*/false));
            return windows_.back().get();
          }));
    }
    SetSwapWindowFactoryForTesting(base::BindLambdaForTesting(
        [this](Profile* profile, const gfx::Rect& bounds, bool maximized) {
          windows_.push_back(CreateBrowser(profile,
                                           BrowserWindowInterface::TYPE_NORMAL,
                                           /*hosted_app=*/false));
          return windows_.back().get();
        }));
    // The directory lists the profiles in a posted task.
    Settle();
    other_home_ = &other_list().active_space();
  }

  void TearDown() override {
    SetSwapWindowFactoryForTesting({});
    for (auto& window : windows_) {
      window->GetTabStripModel()->CloseAllTabs();
    }
    windows_.clear();
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

  SpaceList& list() { return SpaceService::Get(profile())->space_list(); }
  SpaceList& other_list() {
    return SpaceService::Get(other_profile_)->space_list();
  }
  BrowserWindowInterface* parking() {
    return SpaceService::Get(profile())->parking_window();
  }

  raw_ptr<TestingProfile> other_profile_ = nullptr;
  raw_ptr<SpaceModel> other_home_ = nullptr;
  std::vector<std::unique_ptr<BrowserWindowInterface>> windows_;
};

TEST_F(SpaceSwapTest, ListsEveryProfilesSpaces) {
  SpaceModel* work = other_list().AddSpace(1, u"Work");

  std::vector<SpaceDirectory::Entry> spaces = GetSwitchableSpaces(browser());

  ASSERT_EQ(spaces.size(), 3u);
  // Each profile's spaces together, in their order.
  const bool own_first = spaces[0].profile == profile();
  const size_t other = own_first ? 1 : 0;
  EXPECT_EQ(spaces[other].space, other_home_);
  EXPECT_EQ(spaces[other + 1].space, work);
  EXPECT_EQ(spaces[own_first ? 0 : 2].space, &list().active_space());
  EXPECT_EQ(FindSwitchableSpace(browser(), work->id()).profile,
            other_profile_.get());
}

TEST_F(SpaceSwapTest, OffTheRecordWindowsListOnlyTheirOwn) {
  Profile* incognito =
      profile()->GetPrimaryOTRProfile(/*create_if_needed=*/true);
  std::unique_ptr<BrowserWindowInterface> window = CreateBrowser(
      incognito, BrowserWindowInterface::TYPE_NORMAL, /*hosted_app=*/false);

  std::vector<SpaceDirectory::Entry> spaces =
      GetSwitchableSpaces(window.get());

  ASSERT_EQ(spaces.size(), 1u);
  EXPECT_EQ(spaces[0].profile, incognito);
  // And the others don't list it.
  for (const SpaceDirectory::Entry& entry : GetSwitchableSpaces(browser())) {
    EXPECT_NE(entry.profile, incognito);
  }
  window->GetTabStripModel()->CloseAllTabs();
}

TEST_F(SpaceSwapTest, SwapsTheWindowAndParksItsTabs) {
  OpenTabs(browser(), {"a.test", "b.test"});
  SpaceModel* work = other_list().AddSpace(1, u"Work");
  work->AddTab(work->pinned_root(), 0, GURL("https://w.test/"), u"W");

  BrowserWindowInterface* window =
      SwitchWindowToSpace(browser(), other_profile_, *work);
  Settle();

  ASSERT_TRUE(window);
  ASSERT_NE(window, browser());
  EXPECT_EQ(window->GetProfile(), other_profile_.get());
  EXPECT_EQ(&WindowFeatures::From(window)->space_model(), work);
  EXPECT_EQ(&other_list().active_space(), work);
  // The new window opened the space's first entry.
  ASSERT_EQ(window->GetTabStripModel()->count(), 1);
  EXPECT_EQ(WindowFeatures::From(window)->tab_strip_bridge().GetNode(
                window->GetTabStripModel()->GetActiveTab()),
            work->pinned_root()->children()[0].get());
  // The old window emptied; its tabs are parked in their profile.
  EXPECT_TRUE(browser()->GetTabStripModel()->empty());
  EXPECT_TRUE(WindowFeatures::From(browser())->swapped_away());
  ASSERT_TRUE(parking());
  EXPECT_THAT(Hosts(parking()->GetTabStripModel()),
              ElementsAre("a.test", "b.test"));
}

TEST_F(SpaceSwapTest, SwappingBackBringsTheTabsBack) {
  OpenTabs(browser(), {"a.test", "b.test"});
  browser()->GetTabStripModel()->ActivateTabAt(1);
  content::WebContents* b = browser()->GetTabStripModel()->GetWebContentsAt(1);
  SpaceModel& home = list().active_space();

  BrowserWindowInterface* other =
      SwitchWindowToSpace(browser(), other_profile_, *other_home_);
  Settle();
  ASSERT_TRUE(other);
  BrowserWindowInterface* back = SwitchWindowToSpace(other, profile(), home);
  Settle();

  ASSERT_TRUE(back);
  EXPECT_EQ(back->GetProfile(), profile());
  EXPECT_THAT(Hosts(back->GetTabStripModel()), ElementsAre("a.test", "b.test"));
  EXPECT_EQ(back->GetTabStripModel()->GetActiveWebContents(), b);
  EXPECT_TRUE(parking()->GetTabStripModel()->empty());
  // The other profile's empty space had a new tab, which closed with its
  // entry.
  EXPECT_TRUE(other->GetTabStripModel()->empty());
  EXPECT_THAT(other_home_->today_root()->children(), IsEmpty());
}

TEST_F(SpaceSwapTest, ParkedTabsStayWhileAnotherProfilesWindowIsOpen) {
  OpenTabs(browser(), {"a.test"});
  BrowserWindowInterface* window =
      SwitchWindowToSpace(browser(), other_profile_, *other_home_);
  ASSERT_TRUE(window);
  BrowserWindowInterface* parked = parking();
  ASSERT_TRUE(parked);

  // The swapped-away window goes; the other profile's still shows.
  std::unique_ptr<BrowserWindowInterface> old = release_browser();
  old.reset();
  Settle();

  EXPECT_EQ(parking(), parked);
  EXPECT_THAT(Hosts(parked->GetTabStripModel()), ElementsAre("a.test"));
}

TEST_F(SpaceSwapTest, SwitchingWithinTheProfileStillRebinds) {
  SpaceModel* work = list().AddSpace(1, u"Work");

  EXPECT_EQ(SwitchWindowToSpace(browser(), profile(), *work), browser());
  EXPECT_EQ(&WindowFeatures::From(browser())->space_model(), work);
}

}  // namespace
}  // namespace strata
