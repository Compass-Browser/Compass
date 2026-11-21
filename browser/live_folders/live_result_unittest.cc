// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/live_folders/live_result.h"

#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

TEST(LiveResultTest, ChecksItemsAgain) {
  auto result = ParseLiveResult(R"json({
    "items": [
      {"id": "1", "title": "One", "url": "https://a.test/1"},
      {"id": "1", "title": "Again", "url": "https://a.test/again"},
      {"id": "2", "title": "Script", "url": "javascript:alert(1)"},
      {"id": "3", "title": "", "url": "https://a.test/3"},
      {"title": "No id", "url": "https://a.test/4"},
      {"id": "5", "title": "Five", "url": "http://a.test/5"}
    ],
    "title": "Feed", "nextRefresh": 600})json");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->items.size(), 2u);
  EXPECT_EQ(result->items[0].key, "1");
  EXPECT_EQ(result->items[0].title, u"One");
  EXPECT_EQ(result->items[1].key, "5");
  EXPECT_EQ(result->title, u"Feed");
  EXPECT_EQ(result->next_refresh, base::Seconds(600));
}

TEST(LiveResultTest, CapsItems) {
  std::string json = R"({"items": [)";
  for (int i = 0; i < 60; ++i) {
    json += (i ? "," : "") + std::string(R"({"id": ")") +
            std::to_string(i) + R"(", "title": "T", "url": "https://a.test/"})";
  }
  json += "]}";
  auto result = ParseLiveResult(json);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->items.size(), kMaxLiveItems);
}

TEST(LiveResultTest, RejectsNonResults) {
  EXPECT_EQ(ParseLiveResult("[]").error().code, "bad_result");
  EXPECT_EQ(ParseLiveResult(R"({"items": {}})").error().code, "bad_result");
  EXPECT_EQ(ParseLiveResult("not json").error().code, "bad_result");
}

// plugins/conformance/preview-result-rules, checked again in the browser.
TEST(LiveResultTest, PreviewRules) {
  std::string json = R"json({"title": "Header", "footer": "Footer",
    "nextRefresh": 60, "items": [
    {"id": "0", "title": "With url", "url": "https://example.com/0",
     "time": 1795252320000, "badge": "PR", "subtitle": "Sub"},
    {"id": "1", "title": "Allowed icon", "icon": "https://static.example.com/i.png"},
    {"id": "2", "title": "Refused icon", "icon": "https://tracker.example.net/i.png"},
    {"id": "3", "title": "Bad url", "url": "javascript:alert(1)"},
    {"id": "1", "title": "Duplicate"})json";
  for (int i = 4; i < 20; ++i) {
    json += R"(, {"id": ")" + std::to_string(i) + R"(", "title": "T"})";
  }
  json += "]}";
  auto result = ParseLivePreviewResult(json, [](const GURL& icon) {
    return icon.host() == "static.example.com";
  });
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->items.size(), kMaxLivePreviewItems);
  EXPECT_EQ(result->items[0].url, GURL("https://example.com/0"));
  EXPECT_EQ(result->items[0].badge, u"PR");
  EXPECT_EQ(result->items[0].subtitle, u"Sub");
  EXPECT_EQ(result->items[0].time,
            base::Time::FromMillisecondsSinceUnixEpoch(1795252320000));
  EXPECT_TRUE(result->items[1].url.is_empty());
  EXPECT_EQ(result->items[1].icon, GURL("https://static.example.com/i.png"));
  EXPECT_TRUE(result->items[2].icon.is_empty());
  EXPECT_EQ(result->items[3].id, "4");  // "3" had a javascript: URL.
  EXPECT_EQ(result->title, u"Header");
  EXPECT_EQ(result->footer, u"Footer");
  EXPECT_FALSE(result->empty());

  auto empty = ParseLivePreviewResult(R"({"items": []})",
                                      [](const GURL&) { return true; });
  ASSERT_TRUE(empty.has_value());
  EXPECT_TRUE(empty->empty());
  EXPECT_EQ(empty->count, 0);

  // Badges: a count, capped; or some without a number. Neither is content.
  auto counted = ParseLivePreviewResult(R"({"items": [], "count": 123456})",
                                        [](const GURL&) { return true; });
  ASSERT_TRUE(counted.has_value());
  EXPECT_EQ(counted->count, 99999);
  EXPECT_TRUE(counted->empty());
  auto some = ParseLivePreviewResult(R"({"items": [], "count": true})",
                                     [](const GURL&) { return true; });
  ASSERT_TRUE(some.has_value());
  EXPECT_TRUE(some->some);
  EXPECT_EQ(ParseLivePreviewResult(R"({"items": [], "count": -4})",
                                   [](const GURL&) { return true; })
                ->count,
            0);
  EXPECT_EQ(ParseLivePreviewResult("[]", [](const GURL&) { return true; })
                .error()
                .code,
            "bad_result");
}

}  // namespace
}  // namespace compass
