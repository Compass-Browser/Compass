// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/arc_import/arc_sidebar.h"

#include "base/strings/strcat.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::arc {
namespace {

// A StorableSidebar.json as Arc writes it, cut down: two spaces of two
// profiles, a folder in Pinned, a tab with its own title, a split in Today,
// favorites for both profiles, and an item of a kind Compass doesn't know.
constexpr char kSidebar[] = R"json({
  "version": 1,
  "sidebar": {"containers": [
    {"global": {}},
    {
      "spaces": [
        "s1", {
          "id": "s1", "title": "Home", "profile": {"default": true},
          "containerIDs": ["pinned", "p1", "unpinned", "u1"],
          "customInfo": {"windowTheme": {"primaryColorPalette": {"midTone":
            {"red": -0.38, "green": 0.85, "blue": 0.34, "alpha": 1}}},
            "iconType": {"icon": "planet"}}
        },
        "s2", {
          "id": "s2", "title": "Work",
          "profile": {"custom": {"_0": {"directoryBasename": "Profile 1",
                                        "machineID": "M"}}},
          "containerIDs": ["pinned", "p2", "unpinned", "u2"]
        }
      ],
      "items": [
        "p1", {"id": "p1", "data": {"itemContainer": {}},
               "childrenIds": ["f1", "t3"]},
        "f1", {"id": "f1", "title": "Reading", "data": {"list": {}},
               "childrenIds": ["t1", "t2"]},
        "t1", {"id": "t1", "title": null, "data": {"tab": {
               "savedURL": "https://a.test/", "savedTitle": "A",
               "timeLastActiveAt": 700000000, "savedMuteStatus": "allowAudio"}}},
        "t2", {"id": "t2", "title": "My B", "data": {"tab": {
               "savedURL": "https://b.test/", "savedTitle": "B",
               "savedMuteStatus": "muted"}}},
        "t3", {"id": "t3", "data": {"easel": {}}},
        "u1", {"id": "u1", "data": {"itemContainer": {}},
               "childrenIds": ["sp1", "t6"]},
        "sp1", {"id": "sp1", "data": {"splitView": {}},
                "childrenIds": ["t4", "t5"]},
        "t4", {"id": "t4", "data": {"tab": {"savedURL": "https://c.test/"}}},
        "t5", {"id": "t5", "data": {"tab": {"savedURL": "https://d.test/"}}},
        "t6", {"id": "t6", "data": {"tab": {"savedURL": "not a url"}}},
        "p2", {"id": "p2", "data": {"itemContainer": {}}, "childrenIds": []},
        "u2", {"id": "u2", "data": {"itemContainer": {}},
               "childrenIds": ["t7"]},
        "t7", {"id": "t7", "data": {"tab": {"savedURL": "https://e.test/"}}},
        "top1", {"id": "top1", "data": {"itemContainer": {}},
                 "childrenIds": ["t8"]},
        "t8", {"id": "t8", "data": {"tab": {"savedURL": "https://mail.test/",
               "savedTitle": "Mail"}}},
        "top2", {"id": "top2", "data": {"itemContainer": {}},
                 "childrenIds": []}
      ],
      "topAppsContainerIDs": [
        {"default": true}, "top1",
        {"custom": {"_0": {"directoryBasename": "Profile 1"}}}, "top2"
      ]
    }
  ]}
})json";

constexpr char kLocalState[] = R"json({
  "profile": {"info_cache": {
    "Default": {"name": "Your Arc"},
    "Profile 1": {"name": "Work"},
    "Profile 2": {"name": "Spare"}
  }}
})json";

Sidebar Parse() {
  std::optional<base::Value> sidebar = base::JSONReader::Read(
      kSidebar, base::JSON_ALLOW_TRAILING_COMMAS);
  std::optional<base::Value> local_state = base::JSONReader::Read(kLocalState, base::JSON_PARSE_RFC);
  EXPECT_TRUE(sidebar && local_state);
  std::optional<Sidebar> parsed =
      ParseSidebar(sidebar->GetDict(), &local_state->GetDict());
  EXPECT_TRUE(parsed);
  return parsed.value_or(Sidebar());
}

TEST(ArcSidebarTest, SpacesInOrderWithTheirProfiles) {
  Sidebar sidebar = Parse();
  ASSERT_EQ(sidebar.spaces.size(), 2u);
  EXPECT_EQ(sidebar.spaces[0].id, "s1");
  EXPECT_EQ(sidebar.spaces[0].name, u"Home");
  EXPECT_EQ(sidebar.spaces[0].profile_dir, "Default");
  EXPECT_EQ(sidebar.spaces[0].color, SpaceColor::FromRGB(0, 217, 87));
  EXPECT_EQ(sidebar.spaces[1].name, u"Work");
  EXPECT_EQ(sidebar.spaces[1].profile_dir, "Profile 1");
  // No theme: grey.
  EXPECT_EQ(sidebar.spaces[1].color, SpaceColor::kGrey);
  EXPECT_EQ(sidebar.spaces[0].icon, *SpaceIcon::Symbol("globe"));
  EXPECT_TRUE(sidebar.spaces[1].icon.is_none());
}

// A gradient theme's base colors are the space's color and gradient; a plain
// theme's mid tone its color.
TEST(ArcSidebarTest, GradientsComeFromTheirBaseColors) {
  std::optional<base::Value> sidebar = base::JSONReader::Read(
      R"({"sidebar": {"containers": [{"spaces": ["s", {"id": "s", "title": "S",
          "containerIDs": [], "customInfo": {"windowTheme": {
            "background": {"single": {"_0": {"style": {"color": {"_0": {
              "blendedGradient": {"_0": {"baseColors": [
                {"red": 0.2, "green": 0.4, "blue": 0.95},
                {"red": 0.5, "green": 0.5, "blue": 0.52},
                {"red": 0.9, "green": 0.1, "blue": 0.1},
                {"red": 0.1, "green": 0.9, "blue": 0.1},
                {"red": 0.9, "green": 0.9, "blue": 0.1}]}}}}}}}},
            "primaryColorPalette": {"midTone":
              {"red": 0.1, "green": 0.9, "blue": 0.1}}}}}]}]}})",
      base::JSON_PARSE_RFC);
  std::optional<Sidebar> parsed = ParseSidebar(sidebar->GetDict(), nullptr);
  ASSERT_TRUE(parsed);
  ASSERT_EQ(parsed->spaces.size(), 1u);
  EXPECT_EQ(parsed->spaces[0].color, SpaceColor::FromRGB(51, 102, 242));
  // The grey one is left out, and those past the limit.
  EXPECT_EQ(parsed->spaces[0].gradient,
            (std::vector<SpaceColor>{SpaceColor::FromRGB(230, 26, 26),
                                     SpaceColor::FromRGB(26, 230, 26)}));
}

TEST(ArcSidebarTest, SpaceIcons) {
  auto icon_of = [](std::string_view icon_type) {
    std::optional<base::Value> sidebar = base::JSONReader::Read(
        base::StrCat({R"({"sidebar": {"containers": [{"spaces": ["s", {"id": "s",
            "title": "S", "containerIDs": [], "customInfo": {"iconType": )",
                      icon_type, "}}]}]}}"}),
        base::JSON_PARSE_RFC);
    std::optional<Sidebar> parsed = ParseSidebar(sidebar->GetDict(), nullptr);
    return parsed && parsed->spaces.size() == 1 ? parsed->spaces[0].icon
                                                : SpaceIcon();
  };
  EXPECT_EQ(icon_of(R"({"emoji_v2": "🚑", "emoji": 128657})"),
            *SpaceIcon::Emoji("🚑"));
  // Older spaces have only the code point.
  EXPECT_EQ(icon_of(R"({"emoji": 127744})"), *SpaceIcon::Emoji("🌀"));
  EXPECT_EQ(icon_of(R"({"icon": "github"})"), *SpaceIcon::Symbol("code"));
  EXPECT_EQ(icon_of(R"({"icon": "colorPallete"})"),
            *SpaceIcon::Symbol("palette"));
  // Symbols Compass has nothing like show the space's letter.
  EXPECT_TRUE(icon_of(R"({"icon": "somethingElse"})").is_none());
  EXPECT_TRUE(icon_of(R"({"emoji": -5})").is_none());
}

TEST(ArcSidebarTest, ProfilesWithNamesAndFavorites) {
  Sidebar sidebar = Parse();
  ASSERT_EQ(sidebar.profiles.size(), 3u);
  EXPECT_EQ(sidebar.profiles[0].dir, "Default");
  EXPECT_EQ(sidebar.profiles[0].name, u"Your Arc");
  ASSERT_EQ(sidebar.profiles[0].favorites.size(), 1u);
  EXPECT_EQ(sidebar.profiles[0].favorites[0].url, GURL("https://mail.test/"));
  EXPECT_EQ(sidebar.profiles[1].dir, "Profile 1");
  EXPECT_TRUE(sidebar.profiles[1].favorites.empty());
  // Listed in Local State, without a space.
  EXPECT_EQ(sidebar.profiles[2].name, u"Spare");
}

TEST(ArcSidebarTest, PinnedKeepsFoldersAndTitles) {
  Sidebar sidebar = Parse();
  const std::vector<Item>& pinned = sidebar.spaces[0].pinned;
  // The easel is left out.
  ASSERT_EQ(pinned.size(), 1u);
  EXPECT_EQ(pinned[0].kind, Item::Kind::kFolder);
  EXPECT_EQ(pinned[0].title, u"Reading");
  ASSERT_EQ(pinned[0].children.size(), 2u);
  const Item& a = pinned[0].children[0];
  EXPECT_EQ(a.url, GURL("https://a.test/"));
  EXPECT_EQ(a.saved_title, u"A");
  EXPECT_TRUE(a.title.empty());
  EXPECT_EQ(a.last_active, FromArcTime(700000000));
  EXPECT_FALSE(a.muted);
  const Item& b = pinned[0].children[1];
  EXPECT_EQ(b.title, u"My B");
  EXPECT_TRUE(b.muted);
}

TEST(ArcSidebarTest, TodayKeepsSplitsAndSkipsBadUrls) {
  Sidebar sidebar = Parse();
  const std::vector<Item>& today = sidebar.spaces[0].today;
  ASSERT_EQ(today.size(), 1u);
  EXPECT_EQ(today[0].kind, Item::Kind::kSplit);
  ASSERT_EQ(today[0].children.size(), 2u);
  EXPECT_EQ(today[0].children[1].url, GURL("https://d.test/"));
  ASSERT_EQ(sidebar.spaces[1].today.size(), 1u);
}

TEST(ArcSidebarTest, NotASidebar) {
  base::DictValue empty;
  EXPECT_FALSE(ParseSidebar(empty, nullptr));
}

TEST(ArcSidebarTest, ColorsKeepTheirRGB) {
  EXPECT_EQ(ArcSpaceColor(0.9, 0.1, 0.1), SpaceColor::FromRGB(230, 26, 26));
  EXPECT_EQ(ArcSpaceColor(0.2, 0.4, 0.95), SpaceColor::FromRGB(51, 102, 242));
  // Extended sRGB is clamped.
  EXPECT_EQ(ArcSpaceColor(-0.38, 0.85, 0.34), SpaceColor::FromRGB(0, 217, 87));
  EXPECT_EQ(ArcSpaceColor(0.5, 0.5, 0.52), SpaceColor::kGrey);
  EXPECT_EQ(ArcSpaceColor(0.03, 0.01, 0.05), SpaceColor::kGrey);
}

TEST(ArcSidebarTest, ArcTimesCountFrom2001) {
  EXPECT_EQ(FromArcTime(0).InSecondsFSinceUnixEpoch(), 978307200);
}

// Reads a real Arc sidebar and prints what it found (counts only), to check
// the reader against Arc's current format by hand:
//   ARC_SIDEBAR=~/Library/Application\ Support/Arc/StorableSidebar.json \
//   compass_unittests --gtest_also_run_disabled_tests \
//       --gtest_filter=ArcSidebarTest.DISABLED_RealSidebar
TEST(ArcSidebarTest, DISABLED_RealSidebar) {
  std::optional<std::string> path =
      base::Environment::Create()->GetVar("ARC_SIDEBAR");
  ASSERT_TRUE(path);
  std::string json;
  ASSERT_TRUE(base::ReadFileToString(base::FilePath(*path), &json));
  std::optional<base::Value> value = base::JSONReader::Read(json, 0);
  ASSERT_TRUE(value && value->is_dict());
  std::optional<Sidebar> sidebar = ParseSidebar(value->GetDict(), nullptr);
  ASSERT_TRUE(sidebar);
  auto count = [](const std::vector<Item>& items, auto& self) -> size_t {
    size_t n = 0;
    for (const Item& item : items) {
      n += item.kind == Item::Kind::kTab ? 1 : self(item.children, self);
    }
    return n;
  };
  for (const Space& space : sidebar->spaces) {
    LOG(INFO) << "space profile=" << space.profile_dir
              << " color=" << std::hex
              << space.color.tint().value_or(0) << std::dec
              << " pinned_entries=" << space.pinned.size()
              << " pinned_tabs=" << count(space.pinned, count)
              << " today_tabs=" << count(space.today, count);
  }
  for (const Profile& profile : sidebar->profiles) {
    LOG(INFO) << "profile " << profile.dir
              << " favorites=" << profile.favorites.size();
  }
}

}  // namespace
}  // namespace compass::arc
