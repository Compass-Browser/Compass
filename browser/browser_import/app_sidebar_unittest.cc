// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/app_sidebar.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

TEST(AppSidebarTest, ReadsIso8601Times) {
  base::Time expected;
  ASSERT_TRUE(
      base::Time::FromUTCExploded({2026, 10, 5, 30, 9, 0, 0, 0}, &expected));
  EXPECT_EQ(TimeFromIso8601("2026-10-30T09:00:00Z"), expected);
  EXPECT_EQ(TimeFromIso8601("2026-10-30T09:00:00.250Z"), expected);
  EXPECT_EQ(TimeFromIso8601("2026-10-30T11:00:00+02:00"), expected);
  EXPECT_TRUE(TimeFromIso8601("yesterday").is_null());
  EXPECT_TRUE(TimeFromIso8601("").is_null());
}

TEST(AppSidebarTest, ReadsHexColors) {
  EXPECT_EQ(HexSpaceColor("#2861B3"), SpaceColor::FromRGB(0x28, 0x61, 0xb3));
  EXPECT_EQ(HexSpaceColor("2861B3FF"), SpaceColor::FromRGB(0x28, 0x61, 0xb3));
  // Unsaturated: grey, as Arc's plain themes.
  EXPECT_EQ(HexSpaceColor("#F4F1EA"), SpaceColor::kGrey);
  EXPECT_EQ(HexSpaceColor("blue"), std::nullopt);
}

TEST(AppSidebarTest, IconsFromEmojiOrSymbol) {
  EXPECT_EQ(AppSpaceIcon("🏠", "briefcase"), *SpaceIcon::Emoji("🏠"));
  EXPECT_EQ(AppSpaceIcon("", "briefcase.fill"), *SpaceIcon::Symbol("work"));
  EXPECT_EQ(AppSpaceIcon("", "no.such.symbol"), SpaceIcon());
}

TEST(AppSidebarTest, BuildsFoldersWhereTheirFirstEntryComes) {
  FolderTreeBuilder builder("p:");
  const GURL url("https://example.com/");
  builder.Add({}, MakeTab("a", url, u"A"));
  builder.Add({{"work", u"Work"}}, MakeTab("b", url, u"B"));
  builder.Add({}, MakeTab("c", url, u"C"));
  builder.Add({{"work", u"Work"}, {"docs", u""}}, MakeTab("d", url, u"D"));
  builder.Add({{"work", u"Work"}}, MakeTab("e", url, u"E"));
  std::vector<arc::Item> items = std::move(builder).Build();
  ASSERT_EQ(items.size(), 3u);
  EXPECT_EQ(items[0].id, "a");
  EXPECT_EQ(items[1].id, "p:folder:work");
  EXPECT_EQ(items[1].title, u"Work");
  ASSERT_EQ(items[1].children.size(), 3u);
  EXPECT_EQ(items[1].children[0].id, "b");
  EXPECT_EQ(items[1].children[1].title, u"Folder");
  EXPECT_EQ(items[1].children[1].children[0].id, "d");
  EXPECT_EQ(items[1].children[2].id, "e");
  EXPECT_EQ(items[2].id, "c");
}

TEST(AppSidebarTest, MergesHistoryByURL) {
  const GURL a("https://a.example/");
  const GURL b("https://b.example/");
  const base::Time now = base::Time::Now();
  std::vector<HistoryEntry> entries = {
      {a, u"A old", 2, 0, now - base::Days(2)},
      {b, u"B", 1, 1, now - base::Days(1)},
      {a, u"A", 3, 1, now},
  };
  std::vector<HistoryEntry> merged = MergeHistory(entries, 10);
  ASSERT_EQ(merged.size(), 2u);
  EXPECT_EQ(merged[0].url, a);
  EXPECT_EQ(merged[0].title, u"A");
  EXPECT_EQ(merged[0].visit_count, 5);
  EXPECT_EQ(merged[0].typed_count, 1);
  EXPECT_EQ(merged[0].last_visit, now);
  EXPECT_EQ(merged[1].url, b);

  EXPECT_EQ(MergeHistory(entries, 1).size(), 1u);
}

}  // namespace
}  // namespace compass::browser_import
