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

}  // namespace
}  // namespace compass
