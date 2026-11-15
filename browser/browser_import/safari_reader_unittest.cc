// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/safari_reader.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/values_test_util.h"
#include "build/build_config.h"
#include "sql/database.h"
#include "sql/test/test_helpers.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

// Bookmarks.plist as base::Values (ReadPlist() gives the same).
constexpr char kBookmarks[] = R"({
  "WebBookmarkType": "WebBookmarkTypeList",
  "WebBookmarkUUID": "root",
  "Children": [
    {"Title": "History", "WebBookmarkType": "WebBookmarkTypeProxy",
     "WebBookmarkUUID": "history"},
    {"Title": "BookmarksBar", "WebBookmarkType": "WebBookmarkTypeList",
     "WebBookmarkUUID": "bar", "Children": [
       {"WebBookmarkType": "WebBookmarkTypeLeaf", "WebBookmarkUUID": "b1",
        "URLString": "https://favorite.test/",
        "URIDictionary": {"title": "Favorite"}},
       {"WebBookmarkType": "WebBookmarkTypeList", "WebBookmarkUUID": "b2",
        "Title": "Recipes", "Children": [
          {"WebBookmarkType": "WebBookmarkTypeLeaf",
           "WebBookmarkUUID": "b3", "URLString": "https://food.test/soup",
           "URIDictionary": {"title": "Soup"}}]}
     ]},
    {"Title": "BookmarksMenu", "WebBookmarkType": "WebBookmarkTypeList",
     "WebBookmarkUUID": "menu", "Children": [
       {"WebBookmarkType": "WebBookmarkTypeLeaf", "WebBookmarkUUID": "m1",
        "URLString": "https://menu.test/",
        "URIDictionary": {"title": "Menu link"}}]},
    {"Title": "com.apple.ReadingList", "WebBookmarkType": "WebBookmarkTypeList",
     "WebBookmarkUUID": "reading", "Children": [
       {"WebBookmarkType": "WebBookmarkTypeLeaf", "WebBookmarkUUID": "r1",
        "URLString": "https://article.test/",
        "URIDictionary": {"title": "Article"},
        "ReadingList": {"PreviewText": "..."}}]}
  ]
})";

constexpr char kLastSession[] = R"({
  "SessionVersion": "1.0",
  "SessionWindows": [
    {"SelectedTabIndex": 0, "TabStates": [
      {"TabURL": "https://one.test/", "TabTitle": "One",
       "LastVisitTime": 700000000.0},
      {"TabURL": "https://two.test/", "TabTitle": "Two"}]},
    {"IsPrivateWindow": true, "TabStates": [
      {"TabURL": "https://private.test/", "TabTitle": "Private"}]}
  ]
})";

TEST(SafariReaderTest, FavoritesBarFirstThenMenuThenReadingList) {
  std::vector<arc::Item> items =
      ParseSafariBookmarks(base::test::ParseJsonDict(kBookmarks), "safari:");
  ASSERT_EQ(items.size(), 4u);
  EXPECT_EQ(items[0].url, GURL("https://favorite.test/"));
  EXPECT_EQ(items[0].saved_title, u"Favorite");
  EXPECT_EQ(items[0].id, "safari:bookmark:b1");
  EXPECT_EQ(items[1].title, u"Recipes");
  ASSERT_EQ(items[1].children.size(), 1u);
  EXPECT_EQ(items[2].title, u"Bookmarks Menu");
  EXPECT_EQ(items[3].title, u"Reading List");
  ASSERT_EQ(items[3].children.size(), 1u);
  EXPECT_EQ(items[3].children[0].url, GURL("https://article.test/"));
}

TEST(SafariReaderTest, LastSessionWithoutPrivateWindows) {
  std::vector<SessionTab> tabs =
      ParseSafariLastSession(base::test::ParseJsonDict(kLastSession));
  ASSERT_EQ(tabs.size(), 2u);
  EXPECT_EQ(tabs[0].url, GURL("https://one.test/"));
  EXPECT_EQ(tabs[0].title, u"One");
  EXPECT_EQ(tabs[0].last_active,
            base::Time::FromSecondsSinceUnixEpoch(978307200.0 + 700000000.0));

  SourceProfile profile;
  profile.browser = Browser::kSafari;
  profile.id = "safari:Default";
  ImportedSidebar sidebar = SafariSidebar(
      profile,
      ParseSafariBookmarks(base::test::ParseJsonDict(kBookmarks), "s:"),
      tabs, {});
  ASSERT_EQ(sidebar.spaces.size(), 1u);
  EXPECT_EQ(sidebar.spaces[0].name, u"Safari");
  EXPECT_EQ(CountTabs(sidebar.spaces[0].pinned), 4);
  EXPECT_EQ(sidebar.spaces[0].today.size(), 2u);
}

TEST(SafariReaderTest, History) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath path = dir.GetPath().AppendASCII("History.db");
  {
    sql::Database db(sql::test::kTestTag);
    ASSERT_TRUE(db.Open(path));
    ASSERT_TRUE(db.Execute(
        "CREATE TABLE history_items (id INTEGER PRIMARY KEY, url TEXT, "
        "visit_count INTEGER)"));
    ASSERT_TRUE(db.Execute(
        "CREATE TABLE history_visits (id INTEGER PRIMARY KEY, "
        "history_item INTEGER, visit_time REAL, title TEXT)"));
    ASSERT_TRUE(db.Execute(
        "INSERT INTO history_items VALUES (1, 'https://old.test/', 1), "
        "(2, 'https://new.test/', 2), (3, 'file:///etc/hosts', 1)"));
    ASSERT_TRUE(db.Execute(
        "INSERT INTO history_visits VALUES (1, 1, 600000000, 'Old'), "
        "(2, 2, 650000000, 'New before'), (3, 2, 700000000, 'New'), "
        "(4, 3, 710000000, 'Hosts')"));
  }
  std::vector<HistoryEntry> history = ReadSafariHistory(path, 10);
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history[0].url, GURL("https://new.test/"));
  EXPECT_EQ(history[0].title, u"New");
  EXPECT_EQ(history[0].visit_count, 2);
  EXPECT_EQ(history[1].url, GURL("https://old.test/"));
}

// SafariTabs.db as Safari lays it out (the columns the reader uses; Safari
// 15 and later): Root (0), Safari's own lists without a parent, the default
// profile and another one, tab groups under Root and under a profile, a
// window with its unnamed group.
void WriteSafariTabs(const base::FilePath& path) {
  sql::Database db(sql::test::kTestTag);
  ASSERT_TRUE(db.Open(path));
  ASSERT_TRUE(db.Execute(
      "CREATE TABLE bookmarks (id INTEGER PRIMARY KEY AUTOINCREMENT, "
      "special_id INTEGER DEFAULT 0, parent INTEGER, type INTEGER, "
      "title TEXT, url TEXT, hidden INTEGER DEFAULT 0, "
      "order_index INTEGER NOT NULL, external_uuid TEXT UNIQUE, "
      "deleted INTEGER DEFAULT 0, extra_attributes BLOB DEFAULT NULL, "
      "local_attributes BLOB DEFAULT NULL, subtype INTEGER DEFAULT 0)"));
  ASSERT_TRUE(db.Execute(
      "CREATE TABLE windows (id INTEGER PRIMARY KEY, "
      "active_tab_group_id INTEGER, date_closed REAL, "
      "is_last_session INTEGER DEFAULT 0, local_tab_group_id INTEGER, "
      "private_tab_group_id INTEGER, uuid TEXT NOT NULL UNIQUE)"));
  // id, special_id, parent, type, subtype, hidden, order, uuid, title, url.
  ASSERT_TRUE(db.Execute(
      "INSERT INTO bookmarks (id, special_id, parent, type, subtype, hidden, "
      "order_index, external_uuid, title, url) VALUES "
      "(0, 0, NULL, 1, 0, 0, 0, 'Root', 'Root', NULL), "
      "(1, 6, NULL, 1, 0, 1, 0, 'R', 'Recovered', NULL), "
      "(2, 0, NULL, 1, 0, 1, 0, 'pinned', 'pinned', NULL), "
      "(3, 0, NULL, 1, 0, 1, 0, 'privatePinned', 'privatePinned', NULL), "
      "(4, 0, NULL, 1, 0, 1, 0, 'recentlyClosed', 'recentlyClosed', NULL), "
      "(5, 0, 0, 1, 2, 0, 0, 'DefaultProfile', '', NULL), "
      "(6, 0, 0, 1, 2, 0, 1, 'P-WORK', 'Work', NULL), "
      // Tab groups of the default profile, out of id order.
      "(10, 0, 0, 1, 0, 0, 2, 'G-TRAVEL', 'Travel', NULL), "
      "(11, 0, 0, 1, 0, 0, 1, 'G-RECIPES', 'Recipes', NULL), "
      "(12, 0, 0, 1, 0, 0, 3, 'G-EMPTY', 'Empty', NULL), "
      // Another profile's group.
      "(13, 0, 6, 1, 0, 0, 0, 'G-PROJECT', 'Project', NULL), "
      // A window's unnamed group, and a private one.
      "(20, 0, NULL, 1, 0, 1, 0, 'W-LOCAL', 'Local', NULL), "
      "(21, 0, NULL, 1, 0, 1, 0, 'W-PRIVATE', 'Private', NULL), "
      // Tabs.
      "(30, 0, 11, 0, 0, 0, 1, 'T1', 'Soup', 'https://food.test/soup'), "
      "(31, 0, 11, 0, 0, 0, 0, 'T2', 'Bread', 'https://food.test/bread'), "
      "(32, 0, 11, 0, 0, 0, 2, 'T3', 'Start Page', ''), "
      "(33, 0, 10, 0, 0, 0, 0, 'T4', 'Map', 'https://map.test/'), "
      "(34, 0, 10, 0, 0, 0, 1, 'T5', 'Hotel', 'https://hotel.test/'), "
      "(35, 0, 13, 0, 0, 0, 0, 'T6', 'Tracker', 'https://tracker.test/'), "
      "(36, 0, 20, 0, 0, 0, 0, 'T7', 'News', 'https://news.test/'), "
      "(37, 0, 21, 0, 0, 0, 0, 'T8', 'Secret', 'https://private.test/'), "
      "(38, 0, 2, 0, 0, 0, 0, 'T9', 'Mail', 'https://mail.test/'), "
      "(39, 0, 4, 0, 0, 0, 0, 'T10', 'Closed', 'https://closed.test/'), "
      // A group's favorites.
      "(40, 0, 10, 1, 0, 0, 2, 'F', 'TopScopedBookmarkList', NULL), "
      "(41, 0, 40, 0, 0, 0, 0, 'F1', 'Airline', 'https://air.test/')"));
  // A deleted tab, and a pinned one (its attributes carry PinnedAddress).
  ASSERT_TRUE(db.Execute(
      "INSERT INTO bookmarks (id, parent, type, order_index, external_uuid, "
      "title, url, deleted) VALUES "
      "(50, 10, 0, 5, 'T11', 'Gone', 'https://gone.test/', 1)"));
  ASSERT_TRUE(db.Execute(
      "INSERT INTO bookmarks (id, parent, type, order_index, external_uuid, "
      "title, url, local_attributes) VALUES "
      "(51, 10, 0, 6, 'T12', 'Weather', 'https://weather.test/', "
      "CAST('bplist00..PinnedAddress..PinnedTitle' AS BLOB))"));
  ASSERT_TRUE(db.Execute(
      "INSERT INTO windows (id, active_tab_group_id, date_closed, "
      "is_last_session, local_tab_group_id, private_tab_group_id, uuid) "
      "VALUES (1, 20, NULL, 1, 20, 21, 'W1')"));
}

TEST(SafariReaderTest, TabGroups) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath path = dir.GetPath().AppendASCII("SafariTabs.db");
  WriteSafariTabs(path);

  std::optional<SafariTabs> tabs = ReadSafariTabs(path);
  ASSERT_TRUE(tabs);
  // In Safari's order, the default profile's first; the empty group too.
  ASSERT_EQ(tabs->groups.size(), 4u);
  const SafariTabGroup& recipes = tabs->groups[0];
  EXPECT_EQ(recipes.id, "G-RECIPES");
  EXPECT_EQ(recipes.name, u"Recipes");
  EXPECT_TRUE(recipes.profile.empty());
  ASSERT_EQ(recipes.tabs.size(), 3u);
  EXPECT_EQ(recipes.tabs[0].url, GURL("https://food.test/bread"));
  EXPECT_EQ(recipes.tabs[1].url, GURL("https://food.test/soup"));
  EXPECT_EQ(recipes.tabs[1].title, u"Soup");

  const SafariTabGroup& travel = tabs->groups[1];
  EXPECT_EQ(travel.name, u"Travel");
  // The pinned tab first, the deleted one left out.
  ASSERT_EQ(travel.tabs.size(), 3u);
  EXPECT_EQ(travel.tabs[0].url, GURL("https://weather.test/"));
  EXPECT_TRUE(travel.tabs[0].pinned);
  EXPECT_EQ(travel.tabs[1].url, GURL("https://map.test/"));
  EXPECT_FALSE(travel.tabs[1].pinned);
  EXPECT_EQ(travel.tabs[2].url, GURL("https://hotel.test/"));
  ASSERT_EQ(travel.favorites.size(), 1u);
  EXPECT_EQ(travel.favorites[0].url, GURL("https://air.test/"));

  EXPECT_EQ(tabs->groups[2].name, u"Empty");
  EXPECT_TRUE(tabs->groups[2].tabs.empty());
  EXPECT_EQ(tabs->groups[3].name, u"Project");
  EXPECT_EQ(tabs->groups[3].profile, u"Work");

  // Safari's pinned tabs, then the open window's; not the private window's
  // or the recently closed.
  ASSERT_EQ(tabs->open_tabs.size(), 2u);
  EXPECT_EQ(tabs->open_tabs[0].url, GURL("https://mail.test/"));
  EXPECT_TRUE(tabs->open_tabs[0].pinned);
  EXPECT_EQ(tabs->open_tabs[1].url, GURL("https://news.test/"));

  // Spaces: Safari's, then one per group that has tabs.
  SourceProfile profile;
  profile.browser = Browser::kSafari;
  profile.id = "safari:Default";
  ImportedSidebar sidebar =
      SafariSidebar(profile, {}, tabs->open_tabs, tabs->groups);
  ASSERT_EQ(sidebar.spaces.size(), 4u);
  EXPECT_EQ(sidebar.spaces[0].name, u"Safari");
  EXPECT_EQ(sidebar.spaces[0].today.size(), 2u);
  EXPECT_EQ(sidebar.spaces[1].name, u"Recipes");
  EXPECT_EQ(sidebar.spaces[1].id, "safari:Default:tabgroup:G-RECIPES");
  // The start page is left out.
  ASSERT_EQ(sidebar.spaces[1].today.size(), 2u);
  EXPECT_EQ(sidebar.spaces[1].today[0].url, GURL("https://food.test/bread"));
  EXPECT_TRUE(sidebar.spaces[1].pinned.empty());
  EXPECT_EQ(sidebar.spaces[2].name, u"Travel");
  EXPECT_EQ(sidebar.spaces[2].today.size(), 3u);
  ASSERT_EQ(sidebar.spaces[2].pinned.size(), 1u);
  EXPECT_EQ(sidebar.spaces[2].pinned[0].url, GURL("https://air.test/"));
  EXPECT_EQ(sidebar.spaces[3].name, u"Project – Work");
  EXPECT_EQ(sidebar.spaces[3].today.size(), 1u);
  // The same IDs each time, so importing again finds them.
  EXPECT_EQ(SafariSidebar(profile, {}, tabs->open_tabs, tabs->groups)
                .spaces[3]
                .today[0]
                .id,
            sidebar.spaces[3].today[0].id);
}

TEST(SafariReaderTest, TabGroupsMissingOrNotSafari) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  EXPECT_FALSE(ReadSafariTabs(dir.GetPath().AppendASCII("SafariTabs.db")));

  // A database without Safari's tables.
  const base::FilePath other = dir.GetPath().AppendASCII("Other.db");
  {
    sql::Database db(sql::test::kTestTag);
    ASSERT_TRUE(db.Open(other));
    ASSERT_TRUE(db.Execute("CREATE TABLE things (id INTEGER)"));
  }
  EXPECT_FALSE(ReadSafariTabs(other));
}

TEST(SafariReaderTest, TabGroupsWithoutWindowsTable) {
  // Safari 15's database: groups, no windows table.
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath path = dir.GetPath().AppendASCII("SafariTabs.db");
  {
    sql::Database db(sql::test::kTestTag);
    ASSERT_TRUE(db.Open(path));
    ASSERT_TRUE(db.Execute(
        "CREATE TABLE bookmarks (id INTEGER PRIMARY KEY, special_id INTEGER "
        "DEFAULT 0, parent INTEGER, type INTEGER, title TEXT, url TEXT, "
        "hidden INTEGER DEFAULT 0, order_index INTEGER NOT NULL, "
        "external_uuid TEXT, deleted INTEGER DEFAULT 0, extra_attributes "
        "BLOB, local_attributes BLOB, subtype INTEGER DEFAULT 0)"));
    ASSERT_TRUE(db.Execute(
        "INSERT INTO bookmarks (id, parent, type, order_index, "
        "external_uuid, title, url) VALUES "
        "(0, NULL, 1, 0, 'Root', 'Root', NULL), "
        "(1, 0, 1, 0, 'G', 'Reading', NULL), "
        "(2, 1, 0, 0, 'T', 'Paper', 'https://paper.test/')"));
  }
  std::optional<SafariTabs> tabs = ReadSafariTabs(path);
  ASSERT_TRUE(tabs);
  ASSERT_EQ(tabs->groups.size(), 1u);
  EXPECT_EQ(tabs->groups[0].name, u"Reading");
  ASSERT_EQ(tabs->groups[0].tabs.size(), 1u);
  EXPECT_TRUE(tabs->open_tabs.empty());
}

TEST(SafariReaderTest, TabsPaths) {
  const base::FilePath safari =
      base::FilePath(FILE_PATH_LITERAL("home"))
          .AppendASCII("Library")
          .AppendASCII("Safari");
  std::vector<base::FilePath> paths = SafariTabsPaths(safari);
  ASSERT_EQ(paths.size(), 2u);
  EXPECT_EQ(paths[0], base::FilePath(FILE_PATH_LITERAL("home"))
                          .AppendASCII("Library")
                          .AppendASCII("Containers")
                          .AppendASCII("com.apple.Safari")
                          .AppendASCII("Data")
                          .AppendASCII("Library")
                          .AppendASCII("Safari")
                          .AppendASCII("SafariTabs.db"));
  EXPECT_EQ(paths[1], safari.AppendASCII("SafariTabs.db"));
}

TEST(SafariReaderTest, LastSessionPaths) {
  const base::FilePath safari =
      base::FilePath(FILE_PATH_LITERAL("home"))
          .AppendASCII("Library")
          .AppendASCII("Safari");
  std::vector<base::FilePath> paths = SafariLastSessionPaths(safari);
  ASSERT_EQ(paths.size(), 2u);
  EXPECT_EQ(paths[1], safari.AppendASCII("LastSession.plist"));
}

#if BUILDFLAG(IS_MAC)
TEST(SafariReaderTest, ReadsAPropertyList) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath path = dir.GetPath().AppendASCII("Test.plist");
  ASSERT_TRUE(base::WriteFile(path, R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Children</key>
  <array><dict><key>Title</key><string>BookmarksBar</string>
    <key>Flag</key><true/><key>Count</key><integer>3</integer></dict></array>
</dict>
</plist>)"));
  std::optional<base::DictValue> plist = ReadPlist(path);
  ASSERT_TRUE(plist);
  const base::ListValue* children = plist->FindList("Children");
  ASSERT_TRUE(children);
  ASSERT_EQ(children->size(), 1u);
  const base::DictValue& bar = (*children)[0].GetDict();
  EXPECT_EQ(*bar.FindString("Title"), "BookmarksBar");
  EXPECT_EQ(bar.FindBool("Flag"), true);
  EXPECT_EQ(bar.FindInt("Count"), 3);
  EXPECT_FALSE(ReadPlist(dir.GetPath().AppendASCII("Missing.plist")));
}
#endif

}  // namespace
}  // namespace compass::browser_import
