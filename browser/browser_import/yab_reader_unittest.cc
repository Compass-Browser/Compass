// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/yab_reader.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/strcat.h"
#include "base/test/values_test_util.h"
#include "compass/browser/browser_import/detect.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

constexpr char kSpaces[] = R"([
  {"id": "00000000-0000-0000-0000-000000000001", "name": "Personal",
   "icon": "🏠", "ownSignIns": false},
  {"id": "AAAA", "name": "Work", "icon": "briefcase", "ownSignIns": true,
   "paint": {"swatch": "blue", "color": "#2861B3", "grain": 0.3,
             "colors": [], "transparency": 0}}
])";

// The first space's session.json: a pin, kept tabs in nested folders, a
// pair side by side among today's tabs.
constexpr char kPersonal[] = R"({
  "active": 2,
  "tabs": [
    {"url": "https://mail.example/inbox", "home": "https://mail.example/",
     "title": "Inbox", "pin": "M", "id": "t1"},
    {"url": "https://github.com/x/y/pulls", "home": "https://github.com/x/y",
     "homeTitle": "y", "title": "Pulls", "kept": true, "id": "t2",
     "folder": "F2", "folderName": "Code", "folderOpen": true,
     "folderPath": [{"id": "F1", "name": "Work", "open": true}]},
    {"url": "https://docs.example/", "title": "Docs", "kept": true,
     "id": "t3", "name": "My docs"},
    {"url": "https://news.example/", "title": "HN", "seen": 1764000000,
     "id": "t4"},
    {"url": "https://a.example/", "title": "A", "id": "t5"},
    {"url": "about:blank", "title": "", "id": "t6"},
    {"url": "https://b.example/", "title": "B", "id": "t7"}
  ],
  "pairs": [{"first": 4, "second": 6, "share": 0.5}]
})";

constexpr char kFirst[] = "00000000-0000-0000-0000-000000000001";

SourceProfile Profile(std::string_view key = kFirst) {
  SourceProfile profile;
  profile.browser = Browser::kYab;
  profile.id = base::StrCat({"yab:", key});
  return profile;
}

TEST(YabReaderTest, SessionPaths) {
  const base::FilePath root(FILE_PATH_LITERAL("/Yab"));
  EXPECT_EQ(YabSessionPath(root, "00000000-0000-0000-0000-000000000001"),
            root.AppendASCII("session.json"));
  EXPECT_EQ(YabSessionPath(root, "AAAA"),
            root.AppendASCII("session-AAAA.json"));
  EXPECT_EQ(
      YabSharedPinsPath(root),
      root.AppendASCII("session-00000000-0000-0000-0000-000000000A11.json"));
}

TEST(YabReaderTest, SpacesWithPinsKeptAndTodaysTabs) {
  const base::ListValue spaces = base::test::ParseJsonList(kSpaces);
  const base::DictValue personal = base::test::ParseJsonDict(kPersonal);
  const base::DictValue shared = base::test::ParseJsonDict(R"({"tabs": [
    {"url": "https://cal.example/", "title": "Calendar", "pin": "C"}]})");
  std::vector<arc::Item> bookmarks = {
      MakeTab("b", GURL("https://bookmark.example/"), u"B")};
  ImportedSidebar sidebar = YabSidebar(
      Profile(), &spaces, {{kFirst, &personal}}, &shared, std::move(bookmarks));

  // The first profile's space, then its bookmarks. Work has its own
  // sign-ins: another profile.
  ASSERT_EQ(sidebar.spaces.size(), 2u);
  const arc::Space& first = sidebar.spaces[0];
  EXPECT_EQ(first.name, u"Personal");
  EXPECT_EQ(first.icon, *SpaceIcon::Emoji("🏠"));
  // Pinned: the pin, at its home, then the kept tabs, folders nested.
  ASSERT_EQ(first.pinned.size(), 3u);
  EXPECT_EQ(first.pinned[0].url, GURL("https://mail.example/"));
  EXPECT_EQ(first.pinned[1].title, u"Work");
  const arc::Item& code = first.pinned[1].children[0];
  EXPECT_EQ(code.title, u"Code");
  EXPECT_EQ(code.children[0].url, GURL("https://github.com/x/y"));
  EXPECT_EQ(code.children[0].saved_title, u"y");
  EXPECT_EQ(first.pinned[2].title, u"My docs");
  // Today: the tab, then the pair as a split (the blank tab left out).
  ASSERT_EQ(first.today.size(), 2u);
  EXPECT_EQ(first.today[0].url, GURL("https://news.example/"));
  EXPECT_EQ(first.today[0].last_active,
            base::Time::FromSecondsSinceUnixEpoch(1764000000));
  EXPECT_EQ(first.today[1].kind, arc::Item::Kind::kSplit);
  ASSERT_EQ(first.today[1].children.size(), 2u);
  EXPECT_EQ(first.today[1].children[1].url, GURL("https://b.example/"));

  // The shared pins: Favorites. Bookmarks: one more space.
  ASSERT_EQ(sidebar.favorites.size(), 1u);
  EXPECT_EQ(sidebar.favorites[0].url, GURL("https://cal.example/"));
  EXPECT_EQ(sidebar.spaces[1].name, u"Yab");

  // Work's profile: its space (without its session file, with its paint),
  // no shared pins, no bookmarks.
  ImportedSidebar work_sidebar =
      YabSidebar(Profile("AAAA"), &spaces, {{kFirst, &personal}}, &shared,
                 {MakeTab("b", GURL("https://bookmark.example/"), u"B")});
  ASSERT_EQ(work_sidebar.spaces.size(), 1u);
  const arc::Space& work = work_sidebar.spaces[0];
  EXPECT_EQ(work.name, u"Work");
  EXPECT_EQ(work.icon, *SpaceIcon::Symbol("work"));
  EXPECT_EQ(work.color, SpaceColor::FromRGB(0x28, 0x61, 0xb3));
  EXPECT_DOUBLE_EQ(work.grain, 0.3);
  EXPECT_TRUE(work.pinned.empty());
  EXPECT_TRUE(work_sidebar.favorites.empty());
}

TEST(YabReaderTest, ProfilesOfItsSpaces) {
  const base::ListValue spaces = base::test::ParseJsonList(R"([
    {"id": "00000000-0000-0000-0000-000000000001", "name": "Personal"},
    {"id": "bbbb", "name": "Side", "ownSignIns": false},
    {"id": "aaaa", "name": "Work", "ownSignIns": true},
    {"id": "cccc", "name": "Client", "ownSignIns": true,
     "signInWith": "aaaa"}
  ])");
  std::vector<AppProfile> profiles = ParseYabProfiles(&spaces);
  ASSERT_EQ(profiles.size(), 2u);
  EXPECT_EQ(profiles[0].key, kFirst);
  EXPECT_EQ(profiles[0].spaces,
            (std::vector<std::u16string>{u"Personal", u"Side"}));
  EXPECT_EQ(profiles[1].key, "AAAA");
  EXPECT_EQ(profiles[1].spaces,
            (std::vector<std::u16string>{u"Work", u"Client"}));

  ASSERT_EQ(ParseYabProfiles(nullptr).size(), 1u);
  EXPECT_EQ(ParseYabProfiles(nullptr)[0].key, kFirst);
}

TEST(YabReaderTest, ChromiumFolders) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath app = dir.GetPath();
  const base::FilePath chromium = app.AppendASCII("Chromium");
  // Without a record, the folder named after the profile.
  EXPECT_EQ(YabChromiumFolder(app, "AAAA"), chromium.AppendASCII("space-AAAA"));
  // The folder Yab recorded, once it's there.
  ASSERT_TRUE(base::CreateDirectory(app.AppendASCII("ChromiumProfiles")));
  ASSERT_TRUE(
      base::WriteFile(app.AppendASCII("ChromiumProfiles")
                          .AppendASCII(base::StrCat({kFirst, "-shared.json"})),
                      R"({"version": 2, "logical": "x", "shared": true,
          "physical": "PPPP"})"));
  EXPECT_EQ(YabChromiumFolder(app, kFirst),
            chromium.AppendASCII(base::StrCat({"space-", kFirst})));
  ASSERT_TRUE(base::CreateDirectory(chromium.AppendASCII("space-PPPP")));
  EXPECT_EQ(YabChromiumFolder(app, kFirst), chromium.AppendASCII("space-PPPP"));
}

TEST(YabReaderTest, OneSpaceWithoutSpacesJson) {
  const base::DictValue personal = base::test::ParseJsonDict(kPersonal);
  ImportedSidebar sidebar =
      YabSidebar(Profile(), nullptr, {{kFirst, &personal}}, nullptr, {});
  ASSERT_EQ(sidebar.spaces.size(), 1u);
  EXPECT_EQ(sidebar.spaces[0].name, u"Personal");
  EXPECT_FALSE(sidebar.spaces[0].pinned.empty());
}

TEST(YabReaderTest, BookmarksAndHistory) {
  const base::ListValue bookmarks = base::test::ParseJsonList(R"([
    {"id": "B1", "title": "Site", "url": "https://site.example/"},
    {"id": "B2", "title": "Folder", "children": [
      {"id": "B3", "title": "Inner", "url": "https://inner.example/"}]},
    {"id": "B4", "title": "Empty", "children": []}
  ])");
  std::vector<arc::Item> items = ParseYabBookmarks(bookmarks, "yab:x:");
  ASSERT_EQ(items.size(), 2u);
  EXPECT_EQ(items[0].id, "yab:x:bookmark:B1");
  EXPECT_EQ(items[1].kind, arc::Item::Kind::kFolder);
  EXPECT_EQ(items[1].children[0].url, GURL("https://inner.example/"));

  const base::ListValue history = base::test::ParseJsonList(R"([
    {"url": "https://a.example/", "key": "a.example/", "title": "A",
     "count": 3, "last": 785000000, "typed": 1},
    {"url": "yab://settings", "key": "settings", "title": "", "count": 1,
     "last": 785000000}
  ])");
  std::vector<HistoryEntry> entries = ParseYabHistory(history);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].visit_count, 3);
  EXPECT_EQ(entries[0].typed_count, 1);
  EXPECT_EQ(entries[0].last_visit, arc::FromArcTime(785000000));
}

TEST(YabReaderTest, ReadsItsFolder) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath root = dir.GetPath().AppendASCII("Yab");
  ASSERT_TRUE(base::CreateDirectory(root));
  ASSERT_TRUE(base::WriteFile(root.AppendASCII("spaces.json"), kSpaces));
  ASSERT_TRUE(base::WriteFile(root.AppendASCII("session.json"), kPersonal));
  ASSERT_TRUE(base::WriteFile(root.AppendASCII("history.json"), R"([
    {"url": "https://a.example/", "title": "A", "count": 1, "last": 1}])"));
  ASSERT_TRUE(base::WriteFile(root.AppendASCII("history-AAAA.json"), R"([
    {"url": "https://a.example/", "title": "A2", "count": 2, "last": 2}])"));

  SetRootsForTesting(std::vector<BrowserRoot>{
      {Browser::kYab, dir.GetPath().AppendASCII("Missing")},
      {Browser::kYab, root}});
  std::vector<SourceProfile> profiles = DetectProfiles();
  SetRootsForTesting(std::nullopt);
  // Two profiles: the first, and Work's own sign-ins.
  ASSERT_EQ(profiles.size(), 2u);
  EXPECT_EQ(profiles[0].id, base::StrCat({"yab:", kFirst}));
  EXPECT_EQ(profiles[0].root, root.AppendASCII("Chromium"));
  EXPECT_EQ(profiles[0].path,
            root.AppendASCII("Chromium")
                .AppendASCII(base::StrCat({"space-", kFirst})));
  EXPECT_EQ(SpaceNameFor(profiles[0]), u"Yab – Personal");
  EXPECT_EQ(profiles[1].id, "yab:AAAA");
  EXPECT_EQ(SpaceNameFor(profiles[1]), u"Yab – Work");
  EXPECT_TRUE(CanImportAsProfile(Browser::kYab));

  ImportedSidebar sidebar = ReadYabSidebar(profiles[0], true, true);
  ASSERT_EQ(sidebar.spaces.size(), 1u);
  EXPECT_EQ(sidebar.spaces[0].pinned.size(), 3u);
  EXPECT_EQ(ReadYabSidebar(profiles[1], true, true).spaces.size(), 1u);

  // Each profile's own history; every file for none.
  std::vector<HistoryEntry> history = ReadYabHistory(root, kFirst, 100);
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history[0].title, u"A");
  history = ReadYabHistory(root, "AAAA", 100);
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history[0].title, u"A2");
  history = ReadYabHistory(root, "", 100);
  ASSERT_EQ(history.size(), 1u);
  EXPECT_EQ(history[0].visit_count, 3);
}

}  // namespace
}  // namespace compass::browser_import
