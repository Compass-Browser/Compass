// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/firefox_reader.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/values_test_util.h"
#include "build/build_config.h"
#include "compass/browser/browser_import/detect.h"
#include "compass/browser/browser_import/profile_reader.h"
#include "compass/browser/browser_import/test_fixtures.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

SourceProfile Profile(Browser browser, std::string id) {
  SourceProfile profile;
  profile.browser = browser;
  profile.id = std::move(id);
  profile.name = u"default-release";
  return profile;
}

TEST(FirefoxReaderTest, ProfilesIniWithTheInstallsDefaultFirst) {
  const base::FilePath root(FILE_PATH_LITERAL("root"));
  std::vector<FirefoxProfileInfo> profiles = ParseProfilesIni(R"(
[Install308046B0AF4A39CB]
Default=Profiles/bbbb.default-release
Locked=1

[Profile1]
Name=default
IsRelative=1
Path=Profiles/aaaa.default
Default=1

[Profile0]
Name=default-release
IsRelative=1
Path=Profiles/bbbb.default-release

[General]
StartWithLastProfile=1
Version=2
)",
                                                              root);
  ASSERT_EQ(profiles.size(), 2u);
  EXPECT_EQ(profiles[0].name, u"default-release");
  EXPECT_TRUE(profiles[0].is_default);
  EXPECT_EQ(profiles[0].path,
            root.AppendASCII("Profiles").AppendASCII("bbbb.default-release"));
  EXPECT_EQ(profiles[1].name, u"default");
  EXPECT_FALSE(profiles[1].is_default);

  // Without an install section, Default=1 decides.
  profiles = ParseProfilesIni(
      "[Profile0]\nName=a\nIsRelative=1\nPath=p/a\n"
      "[Profile1]\nName=b\nIsRelative=1\nPath=p/b\nDefault=1\n",
      root);
  ASSERT_EQ(profiles.size(), 2u);
  EXPECT_EQ(profiles[0].name, u"b");
}

TEST(FirefoxReaderTest, MozLz4) {
  const std::string text(1000, 'x');
  std::vector<uint8_t> file = CompressMozLz4ForTesting(text);
  EXPECT_EQ(DecompressMozLz4(file), text);

  // A block with a match: "abcd" then 8 more bytes copied from 4 back.
  const uint8_t matched[] = {'m', 'o', 'z', 'L', 'z', '4', '0', 0,
                             12,  0,   0,   0,
                             0x44, 'a', 'b', 'c', 'd', 4, 0};
  EXPECT_EQ(DecompressMozLz4(matched), "abcdabcdabcd");

  const uint8_t wrong_size[] = {'m', 'o', 'z', 'L', 'z', '4', '0', 0,
                                9,   0,   0,   0,   0x10, 'a'};
  EXPECT_FALSE(DecompressMozLz4(wrong_size));
  const uint8_t bad_offset[] = {'m', 'o', 'z', 'L', 'z', '4', '0', 0,
                                8,   0,   0,   0,   0x14, 'a', 9, 0};
  EXPECT_FALSE(DecompressMozLz4(bad_offset));
  const uint8_t not_mozlz4[] = {'{', '}'};
  EXPECT_FALSE(DecompressMozLz4(not_mozlz4));
}

TEST(FirefoxReaderTest, BookmarksToolbarFirstThenTheOtherRoots) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath places = dir.GetPath().AppendASCII("places.sqlite");
  ASSERT_TRUE(test::WriteFirefoxPlaces(places));
  std::vector<arc::Item> items = ReadFirefoxBookmarks(places, "ff:");
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(items[0].url, GURL("https://toolbar.test/"));
  EXPECT_EQ(items[0].saved_title, u"Toolbar link");
  EXPECT_EQ(items[0].id, "ff:bookmark:toolbarlink1");
  EXPECT_EQ(items[1].kind, arc::Item::Kind::kFolder);
  EXPECT_EQ(items[1].title, u"News");
  ASSERT_EQ(items[1].children.size(), 1u);
  // No title of its own: the page's.
  EXPECT_EQ(items[1].children[0].saved_title, u"Daily");
  // The menu held only a query: no folder for it.
  EXPECT_EQ(items[2].title, u"Other Bookmarks");
  ASSERT_EQ(items[2].children.size(), 1u);
  EXPECT_EQ(items[2].children[0].url, GURL("https://unfiled.test/"));
}

TEST(FirefoxReaderTest, SessionTabsAndGroups) {
  base::DictValue session =
      base::test::ParseJsonDict(test::MakeFirefoxSessionJson());
  std::vector<SessionTab> tabs = ParseFirefoxSessionTabs(session);
  ASSERT_EQ(tabs.size(), 5u);
  EXPECT_TRUE(tabs[0].pinned);
  // The current entry.
  EXPECT_EQ(tabs[1].url, GURL("https://first.test/next"));
  EXPECT_EQ(tabs[1].last_active,
            base::Time::FromMillisecondsSinceUnixEpoch(1700000001000));

  ImportedSidebar sidebar =
      FirefoxSidebar(Profile(Browser::kFirefox, "firefox:p"),
                     {MakeTab("b", GURL("https://toolbar.test/"), u"T")},
                     &session, /*import_tabs=*/true);
  ASSERT_EQ(sidebar.spaces.size(), 1u);
  const arc::Space& space = sidebar.spaces[0];
  EXPECT_EQ(space.name, u"Firefox");
  ASSERT_EQ(space.pinned.size(), 1u);
  // The pinned tab, the first tab, the group; not the new tab page.
  ASSERT_EQ(space.today.size(), 3u);
  EXPECT_EQ(space.today[0].url, GURL("https://pinned.test/"));
  EXPECT_EQ(space.today[2].kind, arc::Item::Kind::kFolder);
  EXPECT_EQ(space.today[2].title, u"Reading");
  EXPECT_EQ(space.today[2].children.size(), 2u);
}

TEST(FirefoxReaderTest, ZenSpacesPinnedFoldersTodayAndEssentials) {
  base::DictValue state =
      base::test::ParseJsonDict(test::MakeZenSessionsJson());
  SourceProfile profile = Profile(Browser::kZen, "zen:p");
  ImportedSidebar sidebar = ZenSidebar(
      profile, {MakeTab("b", GURL("https://bookmark.test/"), u"B")}, &state,
      /*import_tabs=*/true);

  // Zen's spaces in its order, then one for the bookmarks.
  ASSERT_EQ(sidebar.spaces.size(), 3u);
  const arc::Space& work = sidebar.spaces[0];
  EXPECT_EQ(work.name, u"Work");
  EXPECT_EQ(work.id, "zen:p:space:{work}");
  EXPECT_EQ(work.icon.value(), "\xF0\x9F\x92\xBC");
  EXPECT_EQ(work.color, SpaceColor::FromRGB(59, 130, 246));
  ASSERT_EQ(work.gradient.size(), 1u);
  EXPECT_EQ(work.gradient[0], SpaceColor::FromRGB(139, 92, 246));

  // Pinned: the folder Docs (Guide, then Specs with Spec), then the pinned
  // tab at its own page with the user's label.
  ASSERT_EQ(work.pinned.size(), 2u);
  const arc::Item& docs = work.pinned[0];
  EXPECT_EQ(docs.kind, arc::Item::Kind::kFolder);
  EXPECT_EQ(docs.title, u"Docs");
  ASSERT_EQ(docs.children.size(), 2u);
  EXPECT_EQ(docs.children[0].url, GURL("https://docs.test/guide"));
  EXPECT_EQ(docs.children[1].title, u"Specs");
  ASSERT_EQ(docs.children[1].children.size(), 1u);
  EXPECT_EQ(docs.children[1].children[0].url, GURL("https://docs.test/spec"));
  EXPECT_EQ(work.pinned[1].url, GURL("https://board.test/"));
  EXPECT_EQ(work.pinned[1].title, u"Board");
  EXPECT_EQ(work.pinned[1].saved_title, u"Board home");
  ASSERT_EQ(work.today.size(), 1u);
  EXPECT_EQ(work.today[0].url, GURL("https://work.test/open"));

  const arc::Space& play = sidebar.spaces[1];
  EXPECT_EQ(play.name, u"Play");
  EXPECT_TRUE(play.color.is_grey());
  EXPECT_TRUE(play.pinned.empty());
  // Not the empty tab nor the Glance one.
  ASSERT_EQ(play.today.size(), 1u);
  EXPECT_EQ(play.today[0].url, GURL("https://play.test/"));

  EXPECT_EQ(sidebar.spaces[2].name, u"Zen");
  ASSERT_EQ(sidebar.spaces[2].pinned.size(), 1u);

  ASSERT_EQ(sidebar.favorites.size(), 1u);
  EXPECT_EQ(sidebar.favorites[0].url, GURL("https://mail.test/"));
}

TEST(FirefoxReaderTest, ZenWithoutBookmarksHasOnlyItsSpaces) {
  base::DictValue state =
      base::test::ParseJsonDict(test::MakeZenSessionsJson());
  ImportedSidebar sidebar =
      ZenSidebar(Profile(Browser::kZen, "zen:p"), {}, &state, true);
  EXPECT_EQ(sidebar.spaces.size(), 2u);
  // Tabs not chosen: the bookmarks' space alone.
  sidebar = ZenSidebar(Profile(Browser::kZen, "zen:p"),
                       {MakeTab("b", GURL("https://bookmark.test/"), u"B")},
                       &state, false);
  ASSERT_EQ(sidebar.spaces.size(), 1u);
  EXPECT_EQ(sidebar.spaces[0].name, u"Zen");
  EXPECT_TRUE(sidebar.favorites.empty());
}

// A Zen profile on disk: profiles.ini, places.sqlite and
// zen-sessions.jsonlz4, found and read as the importer does.
TEST(FirefoxReaderTest, ZenProfileOnDisk) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath root = dir.GetPath().AppendASCII("zen");
  const base::FilePath profile_dir =
      root.AppendASCII("Profiles").AppendASCII("abcd.Default (release)");
  ASSERT_TRUE(base::CreateDirectory(profile_dir));
  ASSERT_TRUE(base::WriteFile(
      root.AppendASCII("profiles.ini"),
      "[Profile0]\nName=Default (release)\nIsRelative=1\n"
      "Path=Profiles/abcd.Default (release)\nDefault=1\n"));
  ASSERT_TRUE(test::WriteFirefoxPlaces(profile_dir.AppendASCII("places.sqlite")));
  ASSERT_TRUE(base::WriteFile(
      profile_dir.AppendASCII("zen-sessions.jsonlz4"),
      CompressMozLz4ForTesting(test::MakeZenSessionsJson())));
  SetRootsForTesting(std::vector<BrowserRoot>{{Browser::kZen, root}});
  std::vector<SourceProfile> profiles = DetectProfiles();
  SetRootsForTesting(std::nullopt);
  ASSERT_EQ(profiles.size(), 1u);
  EXPECT_EQ(profiles[0].id, "zen:abcd.Default (release)");
  EXPECT_EQ(profiles[0].name, u"Default (release)");
  EXPECT_FALSE(IsRunning(profiles[0]));

  ProfileData data = ReadProfile(profiles[0], ReadOptions());
  ASSERT_EQ(data.sidebar.spaces.size(), 3u);
  EXPECT_EQ(data.sidebar.spaces[0].name, u"Work");
  EXPECT_EQ(data.sidebar.spaces[2].name, u"Zen");
  EXPECT_EQ(CountTabs(data.sidebar.spaces[2].pinned), 3);
  EXPECT_EQ(data.sidebar.favorites.size(), 1u);
}

// Without zen-sessions.jsonlz4, the session store's first window has it.
TEST(FirefoxReaderTest, ZenStateFromTheSessionStore) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const std::string window = test::MakeZenSessionsJson();
  ASSERT_TRUE(base::WriteFile(
      dir.GetPath().AppendASCII("sessionstore.jsonlz4"),
      CompressMozLz4ForTesting("{\"windows\": [" + window + "]}")));
  std::optional<base::DictValue> state = ReadZenSidebarState(dir.GetPath());
  ASSERT_TRUE(state);
  ASSERT_TRUE(state->FindList("spaces"));
  EXPECT_EQ(state->FindList("spaces")->size(), 2u);
  EXPECT_EQ(state->FindList("tabs")->size(), 8u);
}

TEST(FirefoxReaderTest, AppPathFromCompatibilityIni) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(dir.GetPath().AppendASCII("compatibility.ini"),
                              "[Compatibility]\nLastVersion=140.0\n"
                              "LastPlatformDir=/opt/firefox\n"));
  EXPECT_EQ(FirefoxAppPath(dir.GetPath()),
            base::FilePath::FromUTF8Unsafe("/opt/firefox"));
  EXPECT_FALSE(IsFirefoxProfileInUse(dir.GetPath()));
}

}  // namespace
}  // namespace compass::browser_import
