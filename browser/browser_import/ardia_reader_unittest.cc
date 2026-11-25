// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/ardia_reader.h"

#include "base/test/values_test_util.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

// As Ardia writes session.json (version 11): two profiles in Default's
// folder ("default", and "side" sharing its data), one of their own.
constexpr char kSession[] = R"({
  "version": 11,
  "profiles": {
    "default": {"id": "default", "name": "Personal", "color": "plum",
                "icon": null, "createdAt": 0},
    "side": {"id": "side", "name": "Side", "color": "green", "icon": "🌿",
             "dataId": "default", "createdAt": 0},
    "p-work": {"id": "p-work", "name": "Work", "color": "blue",
               "icon": null, "createdAt": 0}
  },
  "profileOrder": ["side", "default", "p-work"],
  "windowOrder": ["w2", "w1"],
  "windows": [
    {"id": "w1", "kind": "main", "profileId": "default",
     "tabIds": ["mail", "reading", "spec", "news", "docs", "sheet", "blank"]},
    {"id": "w2", "kind": "main", "profileId": "default",
     "tabIds": ["mail-copy", "later"]},
    {"id": "w3", "kind": "private", "profileId": "default",
     "tabIds": ["secret"]}
  ],
  "tabs": [
    {"id": "mail", "profileId": "default", "url": "https://mail.example/u/0",
     "pinnedUrl": "https://mail.example/", "title": "Inbox",
     "pin": {"as": "tile"}, "pinKey": "k-mail", "customTitle": null,
     "lastActiveAt": 1790812800000},
    {"id": "mail-copy", "profileId": "default",
     "url": "https://mail.example/u/0", "pinnedUrl": "https://mail.example/",
     "title": "Inbox", "pin": {"as": "tile"}, "pinKey": "k-mail"},
    {"id": "reading", "profileId": "default",
     "url": "https://blog.example/post", "pinnedUrl": "https://blog.example/",
     "title": "Blog", "pin": {"as": "tile", "folder": {"id": "tf",
     "name": "Reading"}}},
    {"id": "spec", "profileId": "default", "url": "https://spec.example/",
     "title": "Spec", "pin": {"as": "row"}, "customTitle": "The spec"},
    {"id": "news", "profileId": "default", "url": "https://news.example/",
     "title": "News", "pin": null},
    {"id": "docs", "profileId": "default", "url": "https://docs.example/",
     "title": "Docs", "pin": null},
    {"id": "sheet", "profileId": "default", "url": "https://sheet.example/",
     "title": "Sheet", "pin": null},
    {"id": "blank", "profileId": "default", "url": "about:blank",
     "title": "", "pin": null},
    {"id": "later", "profileId": "side", "url": "https://later.example/",
     "title": "Later", "pin": null},
    {"id": "secret", "profileId": "default", "url": "https://secret.example/",
     "title": "Secret", "pin": null}
  ],
  "groups": [
    {"id": "g-work", "windowId": "w1", "profileId": "default", "name": "Work",
     "pinned": true, "tabIds": ["spec"]},
    {"id": "g-today", "windowId": "w1", "profileId": "default",
     "name": "Research", "pinned": false, "tabIds": ["docs", "sheet"]}
  ],
  "splits": [
    {"id": "sp", "windowId": "w1", "orientation": "horizontal",
     "slots": [{"size": 0.5, "tabId": "docs"},
               {"size": 0.5, "tabId": "sheet"}]}
  ],
  "parkedPins": {
    "p-work": {
      "tabs": [{"id": "jira", "profileId": "p-work",
                "url": "https://jira.example/", "title": "Jira",
                "pin": {"as": "row"}, "groupId": "pg"}],
      "groups": [{"id": "pg", "name": "Tickets"}]
    }
  }
})";

SourceProfile Profile(const char* dir) {
  SourceProfile profile;
  profile.browser = Browser::kArdia;
  profile.root = base::FilePath(FILE_PATH_LITERAL("/ardia/Chromium"));
  profile.path = profile.root.AppendASCII(dir);
  profile.id = std::string("ardia:") + dir;
  return profile;
}

TEST(ArdiaReaderTest, SessionPaths) {
  const std::vector<base::FilePath> paths =
      ArdiaSessionPaths(Profile("Default"));
  ASSERT_FALSE(paths.empty());
#if BUILDFLAG(IS_LINUX)
  EXPECT_EQ(paths[0], base::FilePath("/ardia/Chromium/Default/Ardia/"
                                     "linux-session.json"));
#else
  EXPECT_EQ(
      paths[0],
      base::FilePath(FILE_PATH_LITERAL("/ardia")).AppendASCII("session.json"));
#endif
}

TEST(ArdiaReaderTest, ProfilesInTheFolderAsSpaces) {
  const base::DictValue session = base::test::ParseJsonDict(kSession);
  ImportedSidebar sidebar =
      ArdiaSidebar(Profile("Default"), session, {}, /*import_tabs=*/true);

  // "side" first, in profileOrder; "p-work" is another folder's.
  ASSERT_EQ(sidebar.spaces.size(), 2u);
  const arc::Space& side = sidebar.spaces[0];
  EXPECT_EQ(side.id, "ardia:Default:space:side");
  EXPECT_EQ(side.name, u"Side");
  EXPECT_EQ(side.icon, *SpaceIcon::Emoji("🌿"));
  EXPECT_EQ(side.color, SpaceColor::kGreen);
  ASSERT_EQ(side.today.size(), 1u);
  EXPECT_EQ(side.today[0].url, GURL("https://later.example/"));

  const arc::Space& personal = sidebar.spaces[1];
  EXPECT_EQ(personal.name, u"Personal");
  EXPECT_FALSE(personal.color.is_grey());
  // Pinned: Top Apps (once, though two windows have the pin; at its pinned
  // page), the Top Apps folder, then the pinned folder.
  ASSERT_EQ(personal.pinned.size(), 3u);
  EXPECT_EQ(personal.pinned[0].url, GURL("https://mail.example/"));
  EXPECT_EQ(personal.pinned[1].kind, arc::Item::Kind::kFolder);
  EXPECT_EQ(personal.pinned[1].title, u"Reading");
  EXPECT_EQ(personal.pinned[1].children[0].url, GURL("https://blog.example/"));
  EXPECT_EQ(personal.pinned[2].title, u"Work");
  ASSERT_EQ(personal.pinned[2].children.size(), 1u);
  EXPECT_EQ(personal.pinned[2].children[0].title, u"The spec");

  // Today: the tab, then the group with its split (the blank tab and the
  // private window's left out).
  ASSERT_EQ(personal.today.size(), 2u);
  EXPECT_EQ(personal.today[0].url, GURL("https://news.example/"));
  EXPECT_EQ(personal.today[1].title, u"Research");
  ASSERT_EQ(personal.today[1].children.size(), 1u);
  const arc::Item& split = personal.today[1].children[0];
  EXPECT_EQ(split.kind, arc::Item::Kind::kSplit);
  ASSERT_EQ(split.children.size(), 2u);
  EXPECT_EQ(split.children[0].url, GURL("https://docs.example/"));
  EXPECT_EQ(split.children[1].url, GURL("https://sheet.example/"));
}

TEST(ArdiaReaderTest, ParkedPinsOfAProfileWithoutAWindow) {
  const base::DictValue session = base::test::ParseJsonDict(kSession);
  std::vector<arc::Item> bookmarks = {
      MakeTab("b", GURL("https://bookmark.example/"), u"B")};
  ImportedSidebar sidebar = ArdiaSidebar(Profile("Profile p-work"), session,
                                         std::move(bookmarks), true);
  ASSERT_EQ(sidebar.spaces.size(), 2u);
  const arc::Space& work = sidebar.spaces[0];
  EXPECT_EQ(work.name, u"Work");
  EXPECT_EQ(work.color, SpaceColor::kBlue);
  ASSERT_EQ(work.pinned.size(), 1u);
  EXPECT_EQ(work.pinned[0].title, u"Tickets");
  EXPECT_EQ(work.pinned[0].children[0].url, GURL("https://jira.example/"));
  EXPECT_TRUE(work.today.empty());
  // Chromium's bookmarks: one more space.
  EXPECT_EQ(sidebar.spaces[1].name, u"Ardia");
}

TEST(ArdiaReaderTest, PinsBeforeVersionFive) {
  const base::DictValue session = base::test::ParseJsonDict(R"({
    "profiles": {"default": {"id": "default", "name": "Me"}},
    "windows": [{"id": "w", "profileId": "default",
                 "tabIds": ["tile", "row"]}],
    "tabs": [
      {"id": "tile", "profileId": "default", "url": "https://a.example/",
       "pinned": true},
      {"id": "row", "profileId": "default", "url": "https://b.example/",
       "pinned": true, "pinnedAsRow": true}
    ]
  })");
  ImportedSidebar sidebar = ArdiaSidebar(Profile("Default"), session, {}, true);
  ASSERT_EQ(sidebar.spaces.size(), 1u);
  ASSERT_EQ(sidebar.spaces[0].pinned.size(), 2u);
  EXPECT_TRUE(sidebar.spaces[0].today.empty());
}

}  // namespace
}  // namespace compass::browser_import
