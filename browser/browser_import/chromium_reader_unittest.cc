// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/chromium_reader.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/test/values_test_util.h"
#include "compass/browser/browser_import/test_fixtures.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

using test::FixtureTab;

base::DictValue Dict(std::string_view json) {
  return base::test::ParseJsonDict(json);
}

SourceProfile Profile(Browser browser = Browser::kChrome) {
  SourceProfile profile;
  profile.browser = browser;
  profile.id = "chrome:Default";
  profile.name = u"Person 1";
  return profile;
}

TEST(ChromiumReaderTest, ProfilesInTheBrowsersOrder) {
  std::vector<ChromiumProfileInfo> profiles = ParseChromiumProfiles(Dict(R"({
    "profile": {
      "info_cache": {
        "Profile 2": {"name": "Work"},
        "Default": {"name": "Home"},
        "Profile 3": {"name": "Guest", "is_ephemeral": true},
        "Profile 1": {"name": "School"}
      },
      "profiles_order": ["Profile 2"]
    }
  })"));
  ASSERT_EQ(profiles.size(), 3u);
  EXPECT_EQ(profiles[0].dir, "Profile 2");
  EXPECT_EQ(profiles[0].name, u"Work");
  EXPECT_EQ(profiles[1].dir, "Default");
  EXPECT_EQ(profiles[2].dir, "Profile 1");
}

TEST(ChromiumReaderTest, BookmarksBarFirstThenOtherBookmarksAsAFolder) {
  std::vector<arc::Item> items =
      ParseChromiumBookmarks(Dict(test::MakeChromiumBookmarksJson()), "p:");
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(items[0].kind, arc::Item::Kind::kTab);
  EXPECT_EQ(items[0].url, GURL("https://bar.test/"));
  EXPECT_EQ(items[0].saved_title, u"Bar link");
  EXPECT_EQ(items[0].id, "p:bookmark:00000000-0000-4000-8000-000000000001");
  EXPECT_EQ(items[1].kind, arc::Item::Kind::kFolder);
  EXPECT_EQ(items[1].title, u"Docs");
  // The bookmarklet doesn't come along.
  ASSERT_EQ(items[1].children.size(), 1u);
  EXPECT_EQ(items[1].children[0].url, GURL("https://docs.test/guide"));
  EXPECT_EQ(items[2].kind, arc::Item::Kind::kFolder);
  EXPECT_EQ(items[2].title, u"Other bookmarks");
  ASSERT_EQ(items[2].children.size(), 1u);
  EXPECT_EQ(items[2].children[0].url, GURL("https://other.test/"));
}

TEST(ChromiumReaderTest, SessionFileGivesTabsInOrder) {
  std::vector<uint8_t> file = test::MakeChromiumSessionFile(
      {{{"https://one.test/", u"One", /*pinned=*/true},
        {"https://two.test/", u"Two"}},
       {{"https://three.test/", u"Three"}}});
  std::optional<std::vector<SessionTab>> tabs = ParseChromiumSessionFile(file);
  ASSERT_TRUE(tabs);
  ASSERT_EQ(tabs->size(), 3u);
  EXPECT_EQ((*tabs)[0].url, GURL("https://one.test/"));
  EXPECT_EQ((*tabs)[0].title, u"One");
  EXPECT_TRUE((*tabs)[0].pinned);
  EXPECT_EQ((*tabs)[1].url, GURL("https://two.test/"));
  EXPECT_FALSE((*tabs)[1].pinned);
  EXPECT_EQ((*tabs)[2].url, GURL("https://three.test/"));
}

TEST(ChromiumReaderTest, NotASessionFile) {
  const uint8_t encrypted[] = {'S', 'N', 'S', 'S', 5, 0, 0, 0};
  EXPECT_FALSE(ParseChromiumSessionFile(encrypted));
  const uint8_t junk[] = {1, 2, 3};
  EXPECT_FALSE(ParseChromiumSessionFile(junk));
}

TEST(ChromiumReaderTest, TabGroupsWithATitleBecomeTodayFolders) {
  std::vector<uint8_t> file = test::MakeChromiumSessionFile(
      {{{"https://loose.test/", u"Loose"},
        {"https://a.test/", u"A", false, u"Trip"},
        {"https://b.test/", u"B", false, u"Trip"},
        {"chrome://newtab/", u"New Tab"}}});
  std::optional<std::vector<SessionTab>> tabs = ParseChromiumSessionFile(file);
  ASSERT_TRUE(tabs);
  std::vector<arc::Item> today = TodayFromTabs(*tabs, "p:");
  ASSERT_EQ(today.size(), 2u);
  EXPECT_EQ(today[0].url, GURL("https://loose.test/"));
  EXPECT_EQ(today[1].kind, arc::Item::Kind::kFolder);
  EXPECT_EQ(today[1].title, u"Trip");
  ASSERT_EQ(today[1].children.size(), 2u);
  EXPECT_EQ(today[1].children[1].url, GURL("https://b.test/"));
}

TEST(ChromiumReaderTest, ReadsTheNewestSessionFile) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath sessions = dir.GetPath().AppendASCII("Sessions");
  ASSERT_TRUE(base::CreateDirectory(sessions));
  ASSERT_TRUE(base::WriteFile(
      sessions.AppendASCII("Session_13300000000000000"),
      test::MakeChromiumSessionFile({{{"https://old.test/", u"Old"}}})));
  ASSERT_TRUE(base::WriteFile(
      sessions.AppendASCII("Session_13400000000000000"),
      test::MakeChromiumSessionFile({{{"https://new.test/", u"New"}}})));
  std::vector<SessionTab> tabs = ReadChromiumSession(dir.GetPath());
  ASSERT_EQ(tabs.size(), 1u);
  EXPECT_EQ(tabs[0].url, GURL("https://new.test/"));
}

TEST(ChromiumReaderTest, OneSpaceWithBookmarksInPinnedAndTabsInToday) {
  base::DictValue bookmarks = Dict(test::MakeChromiumBookmarksJson());
  std::vector<SessionTab> tabs(1);
  tabs[0].url = GURL("https://open.test/");
  tabs[0].title = u"Open";
  SourceProfile profile = Profile();
  profile.several = true;
  ImportedSidebar sidebar =
      ChromiumSidebar(profile, &bookmarks, nullptr, tabs, true, true);
  ASSERT_EQ(sidebar.spaces.size(), 1u);
  EXPECT_EQ(sidebar.spaces[0].name, u"Chrome – Person 1");
  EXPECT_EQ(CountTabs(sidebar.spaces[0].pinned), 3);
  ASSERT_EQ(sidebar.spaces[0].today.size(), 1u);
  EXPECT_EQ(sidebar.spaces[0].today[0].url, GURL("https://open.test/"));
  EXPECT_TRUE(sidebar.favorites.empty());

  // Only what was chosen.
  sidebar = ChromiumSidebar(Profile(), &bookmarks, nullptr, tabs, false, true);
  EXPECT_EQ(sidebar.spaces[0].name, u"Chrome");
  EXPECT_TRUE(sidebar.spaces[0].pinned.empty());
  EXPECT_EQ(sidebar.spaces[0].today.size(), 1u);
}

TEST(ChromiumReaderTest, VivaldiWorkspacesBecomeSpaces) {
  base::DictValue preferences = Dict(R"({
    "vivaldi": {"workspaces": {"list": [
      {"id": 1700000000001, "name": "Research", "emoji": "🔬"},
      {"id": 1700000000002, "name": "Empty"}
    ]}}
  })");
  std::vector<uint8_t> file = test::MakeChromiumSessionFile(
      {{{"https://main.test/", u"Main"},
        {"https://paper.test/", u"Paper", false, u"", "1700000000001"}}});
  std::optional<std::vector<SessionTab>> tabs = ParseChromiumSessionFile(file);
  ASSERT_TRUE(tabs);
  ASSERT_EQ(tabs->size(), 2u);
  EXPECT_EQ((*tabs)[1].workspace, "1700000000001");

  ImportedSidebar sidebar = ChromiumSidebar(
      Profile(Browser::kVivaldi), nullptr, &preferences, *tabs, true, true);
  ASSERT_EQ(sidebar.spaces.size(), 3u);
  EXPECT_EQ(sidebar.spaces[0].name, u"Vivaldi");
  ASSERT_EQ(sidebar.spaces[0].today.size(), 1u);
  EXPECT_EQ(sidebar.spaces[0].today[0].url, GURL("https://main.test/"));
  EXPECT_EQ(sidebar.spaces[1].name, u"Research");
  EXPECT_EQ(sidebar.spaces[1].icon.value(), "\xF0\x9F\x94\xAC");
  ASSERT_EQ(sidebar.spaces[1].today.size(), 1u);
  EXPECT_EQ(sidebar.spaces[1].today[0].url, GURL("https://paper.test/"));
  EXPECT_EQ(sidebar.spaces[2].name, u"Empty");
  EXPECT_TRUE(sidebar.spaces[2].today.empty());
}

TEST(ChromiumReaderTest, HistoryAndLogins) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  ASSERT_TRUE(test::WriteChromiumUserData(
      dir.GetPath(), "Person 1", {{"https://open.test/", u"Open"}},
      base::BindRepeating(
          [](const std::string& value) { return "x:" + value; })));
  const base::FilePath profile = dir.GetPath().AppendASCII("Default");

  std::vector<HistoryEntry> history =
      ReadChromiumHistory(profile.AppendASCII("History"), 10);
  ASSERT_EQ(history.size(), 2u);
  EXPECT_EQ(history[0].url, GURL("https://history.test/a"));
  EXPECT_EQ(history[0].visit_count, 3);
  EXPECT_GT(history[0].last_visit, history[1].last_visit);
  EXPECT_EQ(ReadChromiumHistory(profile.AppendASCII("History"), 1).size(), 1u);

  int undecryptable = 0;
  std::vector<LoginEntry> logins = ReadChromiumLogins(
      profile.AppendASCII("Login Data"),
      base::BindRepeating(
          [](std::string_view value) -> std::optional<std::string> {
            if (!value.starts_with("x:")) {
              return std::nullopt;
            }
            return std::string(value.substr(2));
          }),
      &undecryptable);
  ASSERT_EQ(logins.size(), 1u);
  EXPECT_EQ(undecryptable, 0);
  EXPECT_EQ(logins[0].url, GURL("https://login.test/"));
  EXPECT_EQ(logins[0].username, u"user");
  EXPECT_EQ(logins[0].password, u"secret");
  EXPECT_EQ(logins[0].username_element, u"email");

  // A key that doesn't fit: counted, not imported.
  logins = ReadChromiumLogins(
      profile.AppendASCII("Login Data"),
      base::BindRepeating([](std::string_view) -> std::optional<std::string> {
        return std::nullopt;
      }),
      &undecryptable);
  EXPECT_TRUE(logins.empty());
  EXPECT_EQ(undecryptable, 1);
}

TEST(ChromiumReaderTest, NotRunningWithoutALock) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  EXPECT_FALSE(IsChromiumRunning(dir.GetPath()));
}

}  // namespace
}  // namespace compass::browser_import
