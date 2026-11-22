// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/abp_preprocessor.h"

#include <string>
#include <vector>

#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::adblock {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;

std::vector<std::string> Rules(std::string_view line) {
  std::vector<std::string> out;
  EXPECT_EQ(PreprocessLine(line, out), LineKind::kRules) << line;
  return out;
}

LineKind Kind(std::string_view line) {
  std::vector<std::string> out;
  return PreprocessLine(line, out);
}

TEST(AbpPreprocessorTest, Comments) {
  EXPECT_EQ(Kind(""), LineKind::kComment);
  EXPECT_EQ(Kind("   "), LineKind::kComment);
  EXPECT_EQ(Kind("! Title: EasyList"), LineKind::kComment);
  EXPECT_EQ(Kind("[Adblock Plus 2.0]"), LineKind::kComment);
}

TEST(AbpPreprocessorTest, KeepsPlainRules) {
  EXPECT_THAT(Rules("||ads.example^"), ElementsAre("||ads.example^"));
  EXPECT_THAT(Rules("@@||cdn.example^$script,domain=a.com|~b.a.com"),
              ElementsAre("@@||cdn.example^$script,domain=a.com|~b.a.com"));
  EXPECT_THAT(Rules("##.ad-box"), ElementsAre("##.ad-box"));
  EXPECT_THAT(Rules("a.com,b.com##.sponsor"),
              ElementsAre("a.com,b.com##.sponsor"));
  EXPECT_THAT(Rules("a.com#@#.ad-box"), ElementsAre("a.com#@#.ad-box"));
  // Native :has() is CSS a stylesheet applies.
  EXPECT_THAT(Rules("a.com##div:has(> .ad)"),
              ElementsAre("a.com##div:has(> .ad)"));
}

TEST(AbpPreprocessorTest, RewritesUboAliases) {
  EXPECT_THAT(Rules("||t.example^$3p"), ElementsAre("||t.example^$third-party"));
  EXPECT_THAT(Rules("||t.example^$1p"),
              ElementsAre("||t.example^$~third-party"));
  EXPECT_THAT(Rules("||t.example^$~3p"),
              ElementsAre("||t.example^$~third-party"));
  EXPECT_THAT(Rules("||t.example^$xhr,frame,css"),
              ElementsAre("||t.example^$xmlhttprequest,subdocument,stylesheet"));
  EXPECT_THAT(Rules("||t.example^$script,from=a.com"),
              ElementsAre("||t.example^$script,domain=a.com"));
  EXPECT_THAT(Rules("||t.example^$important,script"),
              ElementsAre("||t.example^$script"));
  EXPECT_THAT(Rules("||t.example^$important"), ElementsAre("||t.example^"));
}

TEST(AbpPreprocessorTest, DropsPopups) {
  EXPECT_THAT(Rules("||t.example^$script,popup"),
              ElementsAre("||t.example^$script"));
  EXPECT_EQ(Kind("||t.example^$popup"), LineKind::kUnsupported);
  EXPECT_EQ(Kind("||t.example^$popup,third-party"), LineKind::kUnsupported);
}

TEST(AbpPreprocessorTest, SkipsWhatItCantApply) {
  for (const char* line : {
           "/banner\\d+/$image",
           "@@/^https?:\\/\\/x/",
           "||t.example^$redirect=noopjs",
           "||t.example^$removeparam=utm_source",
           "||t.example^$csp=script-src 'none'",
           "||t.example^$domain=google.*",
           "example.com##+js(set-constant, x, 1)",
           "example.com#?#div:has-text(Sponsored)",
           "example.com##div:-abp-has(.ad)",
           "example.com##.ad:upward(2)",
           "example.com#$#abort-on-property-read x",
           "example.com#%#window.x=1",
           "example.com##^script:has-text(ad)",
           "example.com$$script[data-ad]",
           "google.*##.ad",
           "#@#.ad-box",
           "~a.com#@#.ad-box",
           "||реклама.example^",
       }) {
    EXPECT_EQ(Kind(line), LineKind::kUnsupported) << line;
  }
}

TEST(AbpPreprocessorTest, SplitsExcludedDomains) {
  EXPECT_THAT(Rules("a.com,~b.a.com##.ad"),
              ElementsAre("a.com##.ad", "b.a.com#@#.ad"));
  EXPECT_THAT(Rules("~x.com,~y.com##.ad"),
              ElementsAre("##.ad", "x.com#@#.ad", "y.com#@#.ad"));
  EXPECT_THAT(Rules("a.com,~b.com#@#.ad"), ElementsAre("a.com#@#.ad"));
}

TEST(AbpPreprocessorTest, FindsTitle) {
  EXPECT_EQ(FindListTitle("[Adblock Plus 2.0]\n! Version: 1\n"
                          "! Title: My List  \n||a^\n"),
            "My List");
  EXPECT_EQ(FindListTitle("!title:lower\n"), "lower");
  EXPECT_THAT(FindListTitle("||a^\n||b^\n"), IsEmpty());
}

}  // namespace
}  // namespace compass::adblock
