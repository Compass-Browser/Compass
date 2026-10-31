// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/sidebar_import.h"

#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::arc {
namespace {

Item Tab(const std::string& url,
         std::u16string saved_title = u"",
         std::u16string title = u"") {
  Item item;
  item.kind = Item::Kind::kTab;
  item.url = GURL(url);
  item.saved_title = std::move(saved_title);
  item.title = std::move(title);
  item.last_active = base::Time::FromSecondsSinceUnixEpoch(1000);
  return item;
}

Item Group(Item::Kind kind, std::u16string title, std::vector<Item> children) {
  Item item;
  item.kind = kind;
  item.title = std::move(title);
  item.children = std::move(children);
  return item;
}

const base::Time kNow = base::Time::FromSecondsSinceUnixEpoch(5000);

class SidebarImportTest : public testing::Test {
 protected:
  SidebarImportTest() {
    Space home;
    home.name = u"Arc Home";
    home.color = SpaceColor::kGreen;
    home.pinned = {Group(Item::Kind::kFolder, u"Reading",
                         {Tab("https://a.test/", u"A"),
                          Tab("https://b.test/", u"B", u"My B")})};
    home.today = {Group(Item::Kind::kSplit, u"",
                        {Tab("https://c.test/"), Tab("https://d.test/"),
                         Tab("https://e.test/")})};
    Space work;
    work.name = u"Work";
    work.color = SpaceColor::kBlue;
    work.today = {Tab("https://f.test/", u"F")};
    spaces_ = {home, work};
    profile_.favorites = {Tab("https://mail.test/", u"Mail")};
  }

  SpaceList list_;
  Profile profile_;
  std::vector<Space> spaces_;
};

TEST_F(SidebarImportTest, ANewProfilesSpaceTakesTheFirstOnesPlace) {
  std::vector<SpaceModel*> added =
      AddArcSidebar(list_, profile_, spaces_, kNow);
  ASSERT_EQ(list_.spaces().size(), 2u);
  ASSERT_EQ(added.size(), 2u);
  EXPECT_EQ(added[0], list_.spaces()[0].get());
  EXPECT_EQ(list_.spaces()[0]->name(), u"Arc Home");
  EXPECT_EQ(list_.spaces()[0]->color(), SpaceColor::kGreen);
  EXPECT_EQ(list_.spaces()[1]->name(), u"Work");
  EXPECT_EQ(list_.spaces()[1]->color(), SpaceColor::kBlue);
}

TEST_F(SidebarImportTest, AnUsedProfileKeepsItsSpaces) {
  list_.spaces()[0]->AddTab(list_.spaces()[0]->today_root(), 0,
                            GURL("https://mine.test/"), u"Mine");
  AddArcSidebar(list_, profile_, spaces_, kNow);
  ASSERT_EQ(list_.spaces().size(), 3u);
  EXPECT_EQ(list_.spaces()[0]->name(), u"Home");
  EXPECT_EQ(list_.spaces()[1]->name(), u"Arc Home");
}

TEST_F(SidebarImportTest, PinnedKeepsFoldersTitlesAndTimes) {
  AddArcSidebar(list_, profile_, spaces_, kNow);
  const SidebarNode* pinned = list_.spaces()[0]->pinned_root();
  ASSERT_EQ(pinned->children().size(), 1u);
  const SidebarNode* folder = pinned->children()[0].get();
  EXPECT_EQ(folder->type(), SidebarNode::Type::kFolder);
  EXPECT_EQ(folder->title(), u"Reading");
  ASSERT_EQ(folder->children().size(), 2u);
  const SidebarNode* a = folder->children()[0].get();
  EXPECT_EQ(a->url(), GURL("https://a.test/"));
  EXPECT_EQ(a->title(), u"A");
  EXPECT_EQ(a->last_active(), base::Time::FromSecondsSinceUnixEpoch(1000));
  EXPECT_EQ(folder->children()[1]->custom_title(), u"My B");
}

TEST_F(SidebarImportTest, TodayKeepsSplitsAndCountsAsUsedNow) {
  AddArcSidebar(list_, profile_, spaces_, kNow);
  const SidebarNode* today = list_.spaces()[0]->today_root();
  // A split takes two panes; the third tab follows it.
  ASSERT_EQ(today->children().size(), 2u);
  const SidebarNode* split = today->children()[0].get();
  EXPECT_EQ(split->type(), SidebarNode::Type::kSplit);
  ASSERT_EQ(split->children().size(), 2u);
  EXPECT_EQ(split->children()[1]->url(), GURL("https://d.test/"));
  EXPECT_EQ(today->children()[1]->url(), GURL("https://e.test/"));
  EXPECT_EQ(today->children()[1]->last_active(), kNow);
  // A tab without a saved title is named after its host.
  EXPECT_EQ(split->children()[0]->title(), u"c.test");
}

TEST_F(SidebarImportTest, FavoritesAreTabsOnly) {
  profile_.favorites.push_back(
      Group(Item::Kind::kFolder, u"Apps", {Tab("https://cal.test/")}));
  AddArcSidebar(list_, profile_, spaces_, kNow);
  const SidebarNode* favorites = list_.favorites().pinned_root();
  ASSERT_EQ(favorites->children().size(), 2u);
  EXPECT_EQ(favorites->children()[0]->title(), u"Mail");
  EXPECT_EQ(favorites->children()[1]->url(), GURL("https://cal.test/"));
}

}  // namespace
}  // namespace compass::arc
