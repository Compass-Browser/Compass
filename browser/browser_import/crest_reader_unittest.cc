// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/crest_reader.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/values_test_util.h"
#include "compass/browser/browser_import/detect.h"
#include "compass/browser/browser_import/test_fixtures.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

// As Crest writes its "core" row: typed IDs, times in seconds since 2001.
constexpr char kCore[] = R"({
  "spaces": [
    {"id": {"rawValue": "S1"}, "profile": {"id": "P1"}, "name": "Work",
     "symbol": "briefcase.fill", "accent": "teal",
     "branding": {"colors": [{"red": 0.9, "green": 0.2, "blue": 0.2,
                              "alpha": 1}]},
     "folders": [
       {"id": {"rawValue": "F1"}, "location": "saved", "title": "Docs"},
       {"id": {"rawValue": "F2"}, "location": "saved", "title": "Specs",
        "parentID": {"rawValue": "F1"}},
       {"id": {"rawValue": "F3"}, "location": "current", "title": "Trip"},
       {"id": {"rawValue": "F4"}, "location": "saved", "title": "Empty"}
     ],
     "splitGroups": [{"id": {"rawValue": "G1"}, "customTitle": "Compare"}],
     "tabs": [
       {"id": {"rawValue": "T1"}, "title": "Mail",
        "url": "https://mail.example/inbox", "savedURL": "https://mail.example/",
        "placement": "pinned", "lastActivatedAt": 785000000},
       {"id": {"rawValue": "T2"}, "title": "Spec v2",
        "url": "https://spec.example/v2", "savedURL": "https://spec.example/",
        "savedTitle": "Spec", "placement": "saved",
        "folderID": {"rawValue": "F2"}, "customTitle": "The spec"},
       {"id": {"rawValue": "T3"}, "title": "Wiki", "url": "https://wiki.example/",
        "placement": "saved"},
       {"id": {"rawValue": "T4"}, "title": "Downloads",
        "nativeContent": {"kind": "downloads"}, "placement": "current"},
       {"id": {"rawValue": "T5"}, "title": "Flights",
        "url": "https://flights.example/", "placement": "current",
        "folderID": {"rawValue": "F3"}},
       {"id": {"rawValue": "T6"}, "title": "A", "url": "https://a.example/",
        "placement": "current", "splitGroupID": {"rawValue": "G1"}},
       {"id": {"rawValue": "T7"}, "title": "News", "url": "https://news.example/",
        "placement": "current"},
       {"id": {"rawValue": "T8"}, "title": "B", "url": "https://b.example/",
        "placement": "current", "splitGroupID": {"rawValue": "G1"}}
     ],
     "archivedTabs": [], "history": []},
    {"id": {"rawValue": "S2"}, "profile": {"id": "P2"}, "name": "",
     "symbol": "square.grid.2x2.fill", "accent": "rose", "folders": [],
     "tabs": [], "history": []}
  ],
  "defaultSpaceID": {"rawValue": "S1"}
})";

SourceProfile Profile(const char* key) {
  SourceProfile profile;
  profile.browser = Browser::kCrest;
  profile.id = std::string("crest:") + key;
  return profile;
}

TEST(CrestReaderTest, SpacesWithTheirSections) {
  const base::DictValue core = base::test::ParseJsonDict(kCore);
  ImportedSidebar sidebar = CrestSidebar(Profile("P1"), core);

  // Only the spaces of the profile P1.
  ASSERT_EQ(sidebar.spaces.size(), 1u);
  const arc::Space& work = sidebar.spaces[0];
  EXPECT_EQ(work.id, "crest:P1:space:S1");
  EXPECT_EQ(work.name, u"Work");
  EXPECT_EQ(work.icon, *SpaceIcon::Symbol("work"));
  // The branding's color, before the accent.
  EXPECT_EQ(work.color, arc::ArcSpaceColor(0.9, 0.2, 0.2));

  // Pinned: the pinned icon, then saved tabs, folders nested (the empty
  // one left out).
  ASSERT_EQ(work.pinned.size(), 3u);
  EXPECT_EQ(work.pinned[0].url, GURL("https://mail.example/"));
  EXPECT_FALSE(work.pinned[0].last_active.is_null());
  EXPECT_EQ(work.pinned[1].title, u"Docs");
  ASSERT_EQ(work.pinned[1].children.size(), 1u);
  EXPECT_EQ(work.pinned[1].children[0].title, u"Specs");
  const arc::Item& spec = work.pinned[1].children[0].children[0];
  EXPECT_EQ(spec.url, GURL("https://spec.example/"));
  EXPECT_EQ(spec.saved_title, u"Spec");
  EXPECT_EQ(spec.title, u"The spec");
  EXPECT_EQ(work.pinned[2].url, GURL("https://wiki.example/"));

  // Today: the folder, the split where its first tab is (the built-in
  // page left out), then the tab.
  ASSERT_EQ(work.today.size(), 3u);
  EXPECT_EQ(work.today[0].title, u"Trip");
  EXPECT_EQ(work.today[1].kind, arc::Item::Kind::kSplit);
  EXPECT_EQ(work.today[1].title, u"Compare");
  ASSERT_EQ(work.today[1].children.size(), 2u);
  EXPECT_EQ(work.today[1].children[1].url, GURL("https://b.example/"));
  EXPECT_EQ(work.today[2].url, GURL("https://news.example/"));

  // P2's space (any case of its UUID), its color from its accent.
  ImportedSidebar other_sidebar = CrestSidebar(Profile("p2"), core);
  ASSERT_EQ(other_sidebar.spaces.size(), 1u);
  const arc::Space& other = other_sidebar.spaces[0];
  EXPECT_EQ(other.name, u"Space");
  EXPECT_FALSE(other.color.is_grey());
}

TEST(CrestReaderTest, ProfilesOfItsSpaces) {
  base::DictValue core = base::test::ParseJsonDict(kCore);
  // A Sub-Space browses with its group's profile.
  core.FindList("spaces")->Append(
      base::test::ParseJsonDict(R"({"id": {"rawValue": "S3"},
        "profile": {"id": "P3"}, "sharedProfileID": {"rawValue": "p1"},
        "name": "Sub", "tabs": []})"));
  std::vector<AppProfile> profiles = ParseCrestProfiles(core);
  ASSERT_EQ(profiles.size(), 2u);
  EXPECT_EQ(profiles[0].key, "P1");
  EXPECT_EQ(profiles[0].spaces, (std::vector<std::u16string>{u"Work", u"Sub"}));
  EXPECT_EQ(profiles[1].key, "P2");
  EXPECT_EQ(CrestSpaceIds(core, "P1"), (std::vector<std::string>{"S1", "S3"}));
  EXPECT_EQ(AppProfileName(profiles[0].spaces), u"Work, Sub");
}

TEST(CrestReaderTest, ReadsHistory) {
  const base::ListValue history = base::test::ParseJsonList(R"([
    {"id": "H1", "url": "https://a.example/", "title": "A",
     "firstVisitedAt": 780000000, "lastVisitedAt": 785000000,
     "visitCount": 4},
    {"id": "H2", "url": "chrome://settings", "title": "Settings",
     "visitCount": 1}
  ])");
  std::vector<HistoryEntry> entries = ParseCrestHistory(history);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].title, u"A");
  EXPECT_EQ(entries[0].visit_count, 4);
  EXPECT_EQ(entries[0].last_visit, arc::FromArcTime(785000000));
}

TEST(CrestReaderTest, ReadsTheSessionDatabaseAndIsFound) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath root = dir.GetPath().AppendASCII("Crest");
  const base::FilePath path = CrestSessionPath(root);
  ASSERT_TRUE(test::WriteCrestSession(path, kCore,
                                      {{"S1", R"([{"url": "https://a.example/",
                                "visitCount": 2}])"}}));

  std::optional<CrestSession> session =
      ReadCrestSession(path, /*with_history=*/true);
  ASSERT_TRUE(session);
  ASSERT_TRUE(session->core);
  EXPECT_TRUE(session->core->FindList("spaces"));
  ASSERT_EQ(session->history.size(), 1u);
  EXPECT_EQ(session->history.begin()->first, "S1");
  EXPECT_TRUE(ReadCrestSession(path, false)->history.empty());

  SetRootsForTesting(std::vector<BrowserRoot>{{Browser::kCrest, root}});
  std::vector<SourceProfile> profiles = DetectProfiles();
  SetRootsForTesting(std::nullopt);
  // A profile per Crest profile, its Chromium profile as its path.
  ASSERT_EQ(profiles.size(), 2u);
  EXPECT_EQ(profiles[0].id, "crest:P1");
  EXPECT_EQ(profiles[0].root, root.AppendASCII("Chromium"));
  EXPECT_EQ(profiles[0].path,
            root.AppendASCII("Chromium").AppendASCII("Crest-P1"));
  EXPECT_EQ(SpaceNameFor(profiles[0]), u"Crest – Work");
  EXPECT_EQ(profiles[1].id, "crest:P2");
  EXPECT_TRUE(CanImportAsProfile(Browser::kCrest));
}

}  // namespace
}  // namespace compass::browser_import
