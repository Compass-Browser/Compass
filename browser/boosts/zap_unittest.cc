// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/boosts/zap.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace compass::boosts {
namespace {

TEST(ZapTest, NewBoostHidesTheSelectors) {
  const Boost boost = AddZappedSelectors(
      nullptr, "example.com", {"#banner", " .ad > div:nth-of-type(2) "});
  EXPECT_EQ(boost.match, "example.com");
  EXPECT_EQ(boost.name, "Zapped on example.com");
  EXPECT_TRUE(boost.enabled);
  EXPECT_TRUE(boost.js.empty());
  EXPECT_EQ(boost.css,
            "/* Zapped with Compass */\n"
            "#banner { display: none !important; }\n"
            ".ad > div:nth-of-type(2) { display: none !important; }\n");
  EXPECT_TRUE(IsZapBoost(boost));
  EXPECT_EQ(ZappedSelectors(boost),
            (std::vector<std::string>{"#banner", ".ad > div:nth-of-type(2)"}));
}

TEST(ZapTest, AddsToTheSitesBoostOnce) {
  Boost existing = AddZappedSelectors(nullptr, "example.com", {"#banner"});
  existing.id = "f0d5d8f4-4a3c-4bb1-9a56-0e55e6f1d0aa";
  existing.name = "My zaps";
  existing.enabled = false;
  const Boost boost =
      AddZappedSelectors(&existing, "example.com", {"#banner", "footer"});
  EXPECT_EQ(boost.id, existing.id);
  EXPECT_EQ(boost.name, "My zaps");
  EXPECT_TRUE(boost.enabled);
  EXPECT_EQ(ZappedSelectors(boost),
            (std::vector<std::string>{"#banner", "footer"}));
}

TEST(ZapTest, DropsSelectorsThatWouldBreakTheRule) {
  const Boost boost = AddZappedSelectors(
      nullptr, "example.com",
      {"", "a { color: red }", "b /* c */", "p\ndiv", "main"});
  EXPECT_EQ(ZappedSelectors(boost), (std::vector<std::string>{"main"}));
}

TEST(ZapTest, OtherBoostsArentZaps) {
  Boost mine;
  mine.css = "body { background: black; }";
  EXPECT_FALSE(IsZapBoost(mine));
}

}  // namespace
}  // namespace compass::boosts
