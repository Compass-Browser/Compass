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
      tabs);
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
