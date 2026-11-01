// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <vector>

#include "base/strings/utf_string_conversions.h"
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
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/startup_space.h"
#include "compass/browser/window_sync/window_features.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/mojom/window_show_state.mojom.h"
#include "ui/gfx/geometry/rect.h"
#include "url/gurl.h"

namespace compass {
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
    // The tests switch spaces themselves.
    SetShowFirstSpaceAtStartupForTesting(false);
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
        [this](Profile* profile, const gfx::Rect& bounds,
            ui::mojom::WindowShowState show_state) {
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
  SpaceDirectory& directory() { return *SpaceDirectory::Get(); }
  // The other profile has no window: it's listed once asked.
  void ListOther() {
    directory().List(other_profile_);
    Settle();
  }
  // The last window a swap opened, or null.
  BrowserWindowInterface* swapped() {
    for (auto it = windows_.rbegin(); it != windows_.rend(); ++it) {
      if ((*it)->GetType() == BrowserWindowInterface::TYPE_NORMAL) {
        return it->get();
      }
    }
    return nullptr;
  }
  std::vector<SpaceModel*> Spaces() {
    std::vector<SpaceModel*> spaces;
    for (const SpaceDirectory::Entry& entry : directory().GetSpaces()) {
      spaces.push_back(entry.space);
    }
    return spaces;
  }

  raw_ptr<TestingProfile> other_profile_ = nullptr;
  raw_ptr<SpaceModel> other_home_ = nullptr;
  std::vector<std::unique_ptr<BrowserWindowInterface>> windows_;
};

TEST_F(SpaceSwapTest, ListsProfilesWithWindowsInOneOrder) {
  SpaceModel* home = &list().active_space();
  // A profile without a window or a space in the list isn't in it.
  EXPECT_THAT(Spaces(), ElementsAre(home));
  EXPECT_FALSE(directory().IsListed(other_profile_));

  ListOther();
  SpaceModel* work = list().AddSpace(1, u"Work");

  // One list: spaces join at its end, whatever their profile.
  EXPECT_THAT(Spaces(), ElementsAre(home, other_home_.get(), work));
  EXPECT_EQ(FindSwitchableSpace(browser(), other_home_->id()).profile,
            other_profile_.get());
}

TEST_F(SpaceSwapTest, MovingASpaceToAProfileReloadsItThere) {
  OpenTabs(browser(), {"a.test", "b.test"});
  browser()->GetTabStripModel()->ActivateTabAt(1);
  Settle();
  SpaceModel* home = &list().active_space();
  const std::string name = base::UTF16ToUTF8(home->name());

  MoveSpaceToProfile(profile(), *home, other_profile_);
  Settle();

  // The other profile's empty Home stayed out: only the moved space is in
  // the list, now the other profile's, with its entries.
  ASSERT_EQ(Spaces().size(), 1u);
  SpaceModel* moved = Spaces()[0];
  EXPECT_EQ(directory().FindSpace(moved->id()).profile, other_profile_.get());
  EXPECT_EQ(base::UTF16ToUTF8(moved->name()), name);
  EXPECT_EQ(other_list().spaces().size(), 1u);
  ASSERT_EQ(moved->today_root()->children().size(), 2u);
  // The profile it left has no space in the list any more.
  EXPECT_FALSE(directory().IsListed(profile()));
  // The window swapped: the new one loads the entry it showed, in the other
  // profile. The old tabs closed.
  BrowserWindowInterface* window = swapped();
  ASSERT_TRUE(window);
  EXPECT_EQ(window->GetProfile(), other_profile_.get());
  ASSERT_EQ(window->GetTabStripModel()->count(), 1);
  EXPECT_EQ(window->GetTabStripModel()->GetActiveWebContents()->GetVisibleURL(),
            GURL("https://b.test/"));
  EXPECT_EQ(WindowFeatures::From(window)->tab_strip_bridge().GetNode(
                window->GetTabStripModel()->GetActiveTab()),
            moved->today_root()->children()[1].get());
  EXPECT_TRUE(browser()->GetTabStripModel()->empty());
}

TEST_F(SpaceSwapTest, AMovedSpaceKeepsItsPlace) {
  SpaceModel* home = &list().active_space();
  SpaceModel* work = list().AddSpace(1, u"Work");
  SpaceModel* play = list().AddSpace(2, u"Play");
  ListOther();
  ASSERT_THAT(Spaces(), ElementsAre(home, work, play, other_home_.get()));
  work->AddTab(work->pinned_root(), 0, GURL("https://w.test/"), u"W");

  MoveSpaceToProfile(profile(), *work, other_profile_);
  Settle();

  std::vector<SpaceModel*> spaces = Spaces();
  ASSERT_EQ(spaces.size(), 4u);
  EXPECT_EQ(spaces[0], home);
  EXPECT_EQ(directory().FindSpace(spaces[1]->id()).profile,
            other_profile_.get());
  EXPECT_EQ(spaces[1]->name(), u"Work");
  EXPECT_EQ(spaces[1]->pinned_root()->children().size(), 1u);
  EXPECT_EQ(spaces[2], play);
  // Both profiles keep spaces: both stay listed.
  EXPECT_TRUE(directory().IsListed(profile()));
  EXPECT_EQ(list().spaces().size(), 2u);
}

TEST_F(SpaceSwapTest, ReordersSpacesAcrossProfiles) {
  SpaceModel* home = &list().active_space();
  SpaceModel* work = list().AddSpace(1, u"Work");
  ListOther();
  ASSERT_THAT(Spaces(), ElementsAre(home, work, other_home_.get()));

  directory().MoveSpace(*other_home_, 1);
  EXPECT_THAT(Spaces(), ElementsAre(home, other_home_.get(), work));
  directory().MoveSpace(*home, 3);
  EXPECT_THAT(Spaces(), ElementsAre(other_home_.get(), work, home));
  // Next to itself, it stays.
  directory().MoveSpace(*work, 2);
  EXPECT_THAT(Spaces(), ElementsAre(other_home_.get(), work, home));

  // The profile keeps its own order; the file has the new one.
  EXPECT_EQ(list().spaces()[0].get(), home);
  const std::optional<std::string> data = directory().SerializeData();
  ASSERT_TRUE(data);
  EXPECT_LT(data->find(other_home_->id().AsLowercaseString()),
            data->find(home->id().AsLowercaseString()));
}

TEST_F(SpaceSwapTest, DeletingAProfilesLastSpaceUnlistsIt) {
  ListOther();
  ASSERT_EQ(Spaces().size(), 2u);

  DeleteSpace(other_profile_, *other_home_);
  Settle();

  EXPECT_THAT(Spaces(), ElementsAre(&list().active_space()));
  EXPECT_FALSE(directory().IsListed(other_profile_));
}

TEST_F(SpaceSwapTest, DeletingTheShownSpaceSwapsToItsNeighbor) {
  ListOther();
  SpaceModel* home = &list().active_space();
  ASSERT_THAT(Spaces(), ElementsAre(home, other_home_.get()));

  DeleteSpace(profile(), *home);
  Settle();

  ASSERT_TRUE(swapped());
  EXPECT_EQ(&WindowFeatures::From(swapped())->space_model(), other_home_);
  EXPECT_THAT(Spaces(), ElementsAre(other_home_.get()));
  EXPECT_FALSE(directory().IsListed(profile()));
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
  ListOther();
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
  ListOther();
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
  ListOther();
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
}  // namespace compass
