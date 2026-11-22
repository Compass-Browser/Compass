// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/space_import.h"

#include "compass/browser/arc_import/sidebar_import.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

const base::Time kNow = base::Time::FromSecondsSinceUnixEpoch(5000);

ImportedSidebar Sidebar(std::string id, std::u16string name) {
  ImportedSidebar sidebar;
  arc::Space space;
  space.id = id + ":space";
  space.name = std::move(name);
  space.pinned = {
      MakeTab(id + ":bar", GURL("https://bar.test/"), u"Bar"),
      MakeFolder(id + ":docs", u"Docs",
                 {MakeTab(id + ":guide", GURL("https://docs.test/"),
                          u"Guide")})};
  space.today = {MakeTab(id + ":open", GURL("https://open.test/"), u"Open",
                         base::Time::FromSecondsSinceUnixEpoch(10))};
  sidebar.spaces.push_back(std::move(space));
  return sidebar;
}

TEST(SpaceImportTest, AddsASpaceAfterTheUsersWithPinnedAndToday) {
  SpaceList list;
  AddedSidebar added =
      AddImportedSidebar(list, Sidebar("chrome:Default", u"Chrome"), kNow);
  ASSERT_EQ(added.spaces.size(), 1u);
  ASSERT_EQ(list.spaces().size(), 2u);
  EXPECT_EQ(list.spaces()[0]->name(), u"Home");
  SpaceModel* space = list.spaces()[1].get();
  EXPECT_EQ(added.spaces[0], space);
  EXPECT_EQ(space->name(), u"Chrome");
  EXPECT_EQ(added.pinned, 2);
  EXPECT_EQ(added.today, 1);

  const auto& pinned = space->pinned_root()->children();
  ASSERT_EQ(pinned.size(), 2u);
  EXPECT_EQ(pinned[0]->url(), GURL("https://bar.test/"));
  EXPECT_EQ(pinned[1]->type(), SidebarNode::Type::kFolder);
  EXPECT_EQ(pinned[1]->children().size(), 1u);
  const auto& today = space->today_root()->children();
  ASSERT_EQ(today.size(), 1u);
  EXPECT_EQ(today[0]->url(), GURL("https://open.test/"));
}

TEST(SpaceImportTest, ImportedSpacesGetDistinctColors) {
  SpaceList list;
  list.spaces()[0]->SetColor(SpaceColor::kBlue);
  AddImportedSidebar(list, Sidebar("chrome:Default", u"Chrome"), kNow);
  AddImportedSidebar(list, Sidebar("edge:Default", u"Edge"), kNow);
  ASSERT_EQ(list.spaces().size(), 3u);
  EXPECT_FALSE(list.spaces()[1]->color().is_grey());
  EXPECT_FALSE(list.spaces()[2]->color().is_grey());
  EXPECT_NE(list.spaces()[1]->color(), SpaceColor::kBlue);
  EXPECT_NE(list.spaces()[1]->color(), list.spaces()[2]->color());
}

TEST(SpaceImportTest, AColorOfItsOwnIsKept) {
  SpaceList list;
  ImportedSidebar sidebar = Sidebar("zen:p", u"Zen");
  sidebar.spaces[0].color = SpaceColor::FromRGB(10, 20, 30);
  AddImportedSidebar(list, sidebar, kNow);
  EXPECT_EQ(list.spaces()[1]->color(), SpaceColor::FromRGB(10, 20, 30));
}

TEST(SpaceImportTest, ImportingAgainLeavesTheSpaceAlone) {
  SpaceList list;
  AddImportedSidebar(list, Sidebar("chrome:Default", u"Chrome"), kNow);
  EXPECT_TRUE(IsImported(list, Sidebar("chrome:Default", u"").spaces[0]));
  EXPECT_FALSE(IsImported(list, Sidebar("edge:Default", u"").spaces[0]));
  AddedSidebar again =
      AddImportedSidebar(list, Sidebar("chrome:Default", u"Chrome"), kNow);
  EXPECT_TRUE(again.spaces.empty());
  EXPECT_EQ(list.spaces().size(), 2u);
}

TEST(SpaceImportTest, FavoritesAlreadyThereAreLeftOut) {
  SpaceList list;
  list.favorites().AddTab(list.favorites().pinned_root(), 0,
                          GURL("https://mail.test/"), u"Mail");
  ImportedSidebar sidebar;
  sidebar.favorites = {MakeTab("a", GURL("https://mail.test/"), u"Mail"),
                       MakeTab("b", GURL("https://chat.test/"), u"Chat")};
  AddedSidebar added = AddImportedSidebar(list, sidebar, kNow);
  EXPECT_EQ(added.favorites, 1);
  const auto& favorites = list.favorites().pinned_root()->children();
  ASSERT_EQ(favorites.size(), 2u);
  EXPECT_EQ(favorites[1]->url(), GURL("https://chat.test/"));
}

// A profile copied from the browser's: its lone empty space becomes the
// first imported space.
TEST(SpaceImportTest, IntoANewProfileTheEmptySpaceIsReused) {
  SpaceList list;
  const base::Uuid home = list.spaces()[0]->id();
  AddedSidebar added = AddImportedSidebar(
      list, ForNewProfile(Sidebar("chrome:Default", u"Chrome")), kNow,
      /*into_new_profile=*/true);
  ASSERT_EQ(added.spaces.size(), 1u);
  ASSERT_EQ(list.spaces().size(), 1u);
  EXPECT_EQ(list.spaces()[0]->id(), home);
  EXPECT_EQ(list.spaces()[0]->name(), u"Chrome");
  EXPECT_EQ(list.spaces()[0]->pinned_root()->children().size(), 2u);
  EXPECT_EQ(list.spaces()[0]->today_root()->children().size(), 1u);
}

// A space the user already filled stays, and the imported one goes after.
TEST(SpaceImportTest, IntoANewProfileAUsedSpaceStays) {
  SpaceList list;
  AddImportedSidebar(list, Sidebar("safari:", u"Safari"), kNow);
  list.RemoveSpace(*list.spaces()[0]);
  ASSERT_EQ(list.spaces().size(), 1u);
  AddedSidebar added = AddImportedSidebar(
      list, ForNewProfile(Sidebar("chrome:Default", u"Chrome")), kNow,
      /*into_new_profile=*/true);
  ASSERT_EQ(added.spaces.size(), 1u);
  ASSERT_EQ(list.spaces().size(), 2u);
  EXPECT_EQ(list.spaces()[0]->name(), u"Safari");
  EXPECT_EQ(list.spaces()[1]->name(), u"Chrome");
}

// The same browser profile imported as spaces and as a profile gets other
// space IDs in each.
TEST(SpaceImportTest, ANewProfilesIdsAreItsOwn) {
  const ImportedSidebar sidebar = Sidebar("chrome:Default", u"Chrome");
  const ImportedSidebar own = ForNewProfile(sidebar);
  EXPECT_NE(arc::ArcDerivedId(own.spaces[0].id),
            arc::ArcDerivedId(sidebar.spaces[0].id));
  EXPECT_EQ(own.spaces[0].pinned[1].children[0].id,
            "profile/chrome:Default:guide");
}

}  // namespace
}  // namespace compass::browser_import
