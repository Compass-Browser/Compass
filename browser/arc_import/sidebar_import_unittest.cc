// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/sidebar_import.h"

#include "base/uuid.h"
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

TEST_F(SidebarImportTest, MutedTabsStayMuted) {
  spaces_[0].pinned[0].children[0].muted = true;
  spaces_[1].today[0].muted = true;
  profile_.favorites[0].muted = true;
  AddArcSidebar(list_, profile_, spaces_, kNow);
  const SidebarNode* folder =
      list_.spaces()[0]->pinned_root()->children()[0].get();
  EXPECT_TRUE(folder->children()[0]->muted());
  EXPECT_FALSE(folder->children()[1]->muted());
  EXPECT_TRUE(list_.spaces()[1]->today_root()->children()[0]->muted());
  EXPECT_TRUE(list_.favorites().pinned_root()->children()[0]->muted());
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

Item WithId(Item item, std::string id) {
  item.id = std::move(id);
  return item;
}

TEST(SidebarImportIdsTest, DerivedIdsAreStableVersion8Uuids) {
  const base::Uuid id = ArcDerivedId("AB12-CD34");
  ASSERT_TRUE(id.is_valid());
  EXPECT_EQ(id, ArcDerivedId("AB12-CD34"));
  EXPECT_NE(id, ArcDerivedId("AB12-CD35"));
  const std::string text = id.AsLowercaseString();
  ASSERT_EQ(text.size(), 36u);
  EXPECT_EQ(text[14], '8');
  EXPECT_TRUE(text[19] == '8' || text[19] == '9' || text[19] == 'a' ||
              text[19] == 'b');
  EXPECT_FALSE(ArcDerivedId("").is_valid());
}

// The same Arc data imported on two Macs gives the same IDs, so the
// entries merge when the Macs sync (docs/design/sync.md, "Slice 7 notes").
TEST(SidebarImportIdsTest, TwoImportsOfTheSameArcDataShareIds) {
  Space home;
  home.id = "space-home";
  home.name = u"Home";
  Space work;
  work.id = "space-work";
  work.name = u"Work";
  work.pinned = {WithId(Group(Item::Kind::kFolder, u"Docs",
                              {WithId(Tab("https://a.test/"), "tab-a")}),
                        "folder-docs")};
  work.today = {WithId(Group(Item::Kind::kSplit, u"",
                             {WithId(Tab("https://c.test/"), "tab-c"),
                              WithId(Tab("https://d.test/"), "tab-d")}),
                       "split-cd")};
  Profile profile;
  profile.favorites = {WithId(Tab("https://mail.test/"), "fav-mail")};

  SpaceList mac_a;
  SpaceList mac_b;
  AddArcSidebar(mac_a, profile, {home, work}, kNow);
  AddArcSidebar(mac_b, profile, {home, work}, kNow);

  // The first space was the profile's own (it keeps its ID); the second is
  // Arc's.
  ASSERT_EQ(mac_a.spaces().size(), 2u);
  ASSERT_EQ(mac_b.spaces().size(), 2u);
  EXPECT_EQ(mac_a.spaces()[1]->id(), ArcDerivedId("space-work"));
  EXPECT_EQ(mac_a.spaces()[1]->id(), mac_b.spaces()[1]->id());

  const SidebarNode* folder =
      mac_a.spaces()[1]->pinned_root()->children()[0].get();
  EXPECT_EQ(folder->id(), ArcDerivedId("folder-docs"));
  EXPECT_EQ(folder->children()[0]->id(), ArcDerivedId("tab-a"));
  const SidebarNode* split =
      mac_a.spaces()[1]->today_root()->children()[0].get();
  ASSERT_EQ(split->type(), SidebarNode::Type::kSplit);
  EXPECT_EQ(split->id(), ArcDerivedId("split-cd"));
  ASSERT_EQ(split->children().size(), 2u);
  EXPECT_EQ(split->children()[0]->id(), ArcDerivedId("tab-c"));
  EXPECT_EQ(split->children()[1]->id(), ArcDerivedId("tab-d"));
  EXPECT_EQ(mac_b.spaces()[1]->today_root()->children()[0]->id(),
            ArcDerivedId("split-cd"));
  EXPECT_EQ(mac_a.favorites().pinned_root()->children()[0]->id(),
            ArcDerivedId("fav-mail"));

  // A second import on the same Mac takes fresh IDs for what's there.
  AddArcSidebar(mac_a, profile, {home, work}, kNow);
  ASSERT_EQ(mac_a.spaces().size(), 4u);
  EXPECT_NE(mac_a.spaces()[3]->id(), ArcDerivedId("space-work"));
  const SidebarNode* again =
      mac_a.spaces()[3]->pinned_root()->children()[0].get();
  EXPECT_NE(again->id(), ArcDerivedId("folder-docs"));
  EXPECT_EQ(again->children().size(), 1u);
  EXPECT_EQ(mac_a.favorites().pinned_root()->children().size(), 2u);
}

}  // namespace
}  // namespace compass::arc
