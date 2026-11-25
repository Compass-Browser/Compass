// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/cra_reader.h"

#include "base/test/values_test_util.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

// As Cra writes sidebar.json (its recorded test fixture, trimmed): two
// spaces, the second in the profile "Work".
constexpr char kSidebar[] = R"json({
  "version": 1,
  "spaceOrder": ["S2", "S1"],
  "profiles": [{"id": "B1", "name": "Work"}],
  "spaces": [
    {"id": "S1", "title": "Home", "emoji": "🏠",
     "favoritesContainerID": "F1", "pinnedContainerID": "P1",
     "todayContainerID": "T1",
     "theme": {"colors": ["#F4F1EA", "#29283D"], "intensity": 0.6}},
    {"id": "S2", "title": "Side", "emoji": "", "symbol": "briefcase",
     "favoritesContainerID": "F1", "pinnedContainerID": "P2",
     "todayContainerID": "T2",
     "theme": {"accent": "#2861B3", "colors": ["#2861B3"], "intensity": 1,
               "noise": 0.2}},
    {"id": "S3", "title": "Work", "profileID": "B1",
     "favoritesContainerID": "F3", "pinnedContainerID": "P3",
     "todayContainerID": "T3"},
    {"id": "S4", "title": "Blank", "standaloneWindowID": "W1",
     "favoritesContainerID": "F1", "pinnedContainerID": "P4",
     "todayContainerID": "T4"}
  ],
  "items": [
    {"id": "F1", "children": ["fav"],
     "data": {"container": {"_0": {"favorites": {
       "spaceID": "00000000-0000-0000-0000-000000000000"}}}}},
    {"id": "fav", "parentID": "F1", "children": [],
     "data": {"tab": {"_0": {"url": "https://mail.example.com/inbox",
       "savedURL": "https://mail.example.com/", "savedTitle": "Inbox (3)",
               "muted" : true, "timeLastActiveAt" : "2026-10-30T09:00:00Z"
}
}
}
},
    {"id": "P1", "children": ["folder", "blank"],
     "data": {"container": {"_0": {"pinned": {"spaceID": "S1"}}}}},
    {"id": "folder", "parentID": "P1", "title": "Reading",
     "children": ["post"], "data": {"folder": {}}},
    {"id": "post", "parentID": "folder", "title": "My blog", "children": [],
     "data": {"tab": {"_0": {"url": "https://blog.example.org/post/1",
       "savedURL": "https://blog.example.org/", "savedTitle": "Post one",
       "muted": false}}}},
    {"id": "blank", "parentID": "P1", "children": [],
     "data": {"tab": {"_0": {"url": "about:blank", "savedURL": "about:blank",
       "muted": false}}}},
    {"id": "T1", "children": ["split", "shop"],
     "data": {"container": {"_0": {"today": {"spaceID": "S1"}}}}},
    {"id": "split", "parentID": "T1", "children": ["docs", "news"],
     "data": {"split": {"_0": {"orientation": "vertical",
       "widthFactors": ["docs", 0.6]}}}},
    {"id": "docs", "parentID": "split", "children": [],
     "data": {"tab": {"_0": {"url": "https://docs.example.com/a",
       "savedURL": "https://docs.example.com/", "muted": false}}}},
    {"id": "news", "parentID": "split", "children": [],
     "data": {"tab": {"_0": {"url": "https://news.example.com/",
       "muted": false}}}},
    {"id": "shop", "parentID": "T1", "children": [],
     "data": {"tab": {"_0": {"url": "https://shop.example.co.uk/basket",
       "muted": false}}}},
    {"id": "F3", "children": ["own"],
     "data": {"container": {"_0": {"favorites": {"spaceID": "S3"}}}}},
    {"id": "own", "parentID": "F3", "children": [],
     "data": {"tab": {"_0": {"url": "https://work.example/",
       "muted": false}}}},
    {"id": "P3", "children": ["pin"],
     "data": {"container": {"_0": {"pinned": {"spaceID": "S3"}}}}},
    {"id": "pin", "parentID": "P3", "children": [],
     "data": {"tab": {"_0": {"url": "https://jira.example/",
       "muted": false}}}}
  ]
})json";

SourceProfile Profile(const char* dir) {
  SourceProfile profile;
  profile.browser = Browser::kCra;
  profile.root = base::FilePath(FILE_PATH_LITERAL("/Cra/Chromium"));
  profile.path = profile.root.AppendASCII(dir);
  profile.id = std::string("cra:") + dir;
  return profile;
}

TEST(CraReaderTest, SidebarBesideUserData) {
  EXPECT_EQ(CraSidebarPath(base::FilePath(FILE_PATH_LITERAL("/Cra/Chromium"))),
            base::FilePath(FILE_PATH_LITERAL("/Cra/sidebar.json")));
}

TEST(CraReaderTest, DefaultProfilesSpacesInCrasOrder) {
  const base::DictValue document = base::test::ParseJsonDict(kSidebar);
  ImportedSidebar sidebar =
      CraSidebar(Profile("Default"), document, {}, /*import_tabs=*/true);

  // S3 is the other profile's, S4 a Blank Window's; no bookmarks space.
  ASSERT_EQ(sidebar.spaces.size(), 2u);
  const arc::Space& side = sidebar.spaces[0];
  EXPECT_EQ(side.id, "cra:Default:space:S2");
  EXPECT_EQ(side.name, u"Side");
  EXPECT_EQ(side.icon, *SpaceIcon::Symbol("work"));
  EXPECT_EQ(side.color, SpaceColor::FromRGB(0x28, 0x61, 0xb3));
  EXPECT_EQ(side.intensity, 1);
  EXPECT_DOUBLE_EQ(side.grain, 0.2);

  const arc::Space& home = sidebar.spaces[1];
  EXPECT_EQ(home.name, u"Home");
  EXPECT_EQ(home.icon, *SpaceIcon::Emoji("🏠"));
  // An unsaturated theme stays grey (the import gives it a color).
  EXPECT_TRUE(home.color.is_grey());

  // Shared favorites, once, at their saved page.
  ASSERT_EQ(sidebar.favorites.size(), 1u);
  EXPECT_EQ(sidebar.favorites[0].url, GURL("https://mail.example.com/"));
  EXPECT_EQ(sidebar.favorites[0].saved_title, u"Inbox (3)");
  EXPECT_TRUE(sidebar.favorites[0].muted);
  EXPECT_FALSE(sidebar.favorites[0].last_active.is_null());

  // Pinned: the folder (the blank tab left out), its tab at its saved page.
  ASSERT_EQ(home.pinned.size(), 1u);
  EXPECT_EQ(home.pinned[0].kind, arc::Item::Kind::kFolder);
  EXPECT_EQ(home.pinned[0].title, u"Reading");
  ASSERT_EQ(home.pinned[0].children.size(), 1u);
  EXPECT_EQ(home.pinned[0].children[0].url, GURL("https://blog.example.org/"));
  EXPECT_EQ(home.pinned[0].children[0].title, u"My blog");

  // Today: the split, at the pages it was at, then the tab.
  ASSERT_EQ(home.today.size(), 2u);
  EXPECT_EQ(home.today[0].kind, arc::Item::Kind::kSplit);
  ASSERT_EQ(home.today[0].children.size(), 2u);
  EXPECT_EQ(home.today[0].children[0].url, GURL("https://docs.example.com/a"));
  EXPECT_EQ(home.today[1].url, GURL("https://shop.example.co.uk/basket"));
}

TEST(CraReaderTest, AnotherProfilesSpacesWithTheirOwnFavorites) {
  const base::DictValue document = base::test::ParseJsonDict(kSidebar);
  std::vector<arc::Item> bookmarks = {
      MakeTab("b", GURL("https://bookmark.example/"), u"B")};
  ImportedSidebar sidebar =
      CraSidebar(Profile("Profile B1"), document, std::move(bookmarks), true);

  ASSERT_EQ(sidebar.spaces.size(), 2u);
  const arc::Space& work = sidebar.spaces[0];
  EXPECT_EQ(work.name, u"Work");
  // The space's own favorites come first in its Pinned.
  ASSERT_EQ(work.pinned.size(), 2u);
  EXPECT_EQ(work.pinned[0].url, GURL("https://work.example/"));
  EXPECT_EQ(work.pinned[1].url, GURL("https://jira.example/"));
  EXPECT_TRUE(sidebar.favorites.empty());
  // Chromium's bookmarks: one more space.
  EXPECT_EQ(sidebar.spaces[1].name, u"Cra");
  ASSERT_EQ(sidebar.spaces[1].pinned.size(), 1u);
}

TEST(CraReaderTest, WithoutTabsOnlyBookmarks) {
  const base::DictValue document = base::test::ParseJsonDict(kSidebar);
  ImportedSidebar sidebar =
      CraSidebar(Profile("Default"), document, {}, /*import_tabs=*/false);
  ASSERT_EQ(sidebar.spaces.size(), 1u);
  EXPECT_EQ(sidebar.spaces[0].name, u"Cra");
  EXPECT_TRUE(sidebar.favorites.empty());
}

}  // namespace
}  // namespace compass::browser_import
