// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/renderer/adblock/cosmetic_css.h"

#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/strings/string_split.h"
#include "components/subresource_filter/core/common/flat/indexed_ruleset_generated.h"
#include "components/subresource_filter/core/common/indexed_ruleset.h"
#include "components/url_pattern_index/proto/rules.pb.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::adblock {
namespace {

namespace proto = url_pattern_index::proto;
using ::testing::HasSubstr;
using ::testing::Not;

proto::StyleRule Rule(std::vector<std::string> domains,
                      std::string selector,
                      std::vector<std::string> classes,
                      bool exception = false) {
  proto::StyleRule rule;
  rule.set_semantics(exception ? proto::RULE_SEMANTICS_ALLOWLIST
                               : proto::RULE_SEMANTICS_BLOCKLIST);
  for (std::string& domain : domains) {
    rule.add_domains()->set_domain(std::move(domain));
  }
  rule.set_selector(std::move(selector));
  for (std::string& name : classes) {
    rule.add_classes(std::move(name));
  }
  return rule;
}

class CosmeticCssTest : public testing::Test {
 protected:
  void SetUp() override {
    // As the ad blocker's builder orders them: exceptions and site rules,
    // then the rules for every site.
    for (const proto::StyleRule& rule : {
             Rule({"a.test"}, ".ad-box", {"ad-box"}, /*exception=*/true),
             Rule({"a.test"}, ".only-a", {"only-a"}),
             Rule({}, ".ad-box", {"ad-box"}),
             Rule({}, ".banner", {"banner"}),
             Rule({}, "div.sponsor > img", {"sponsor"}),
         }) {
      indexer_.AddStyleRuleFromProto(rule);
    }
    indexer_.Finish();
    index_ = subresource_filter::flat::GetIndexedRuleset(
                 indexer_.data().data())
                 ->style_rule_index();
  }

  subresource_filter::RulesetIndexer indexer_{/*ruleset_id=*/1};
  raw_ptr<const subresource_filter::flat::StyleRuleIndex> index_ = nullptr;
};

TEST_F(CosmeticCssTest, GenericSelectors) {
  GenericSelectors selectors = CollectGenericSelectors(index_.get());
  const std::string css = BuildHidingCss(selectors, {});
  EXPECT_THAT(css, HasSubstr(".ad-box"));
  EXPECT_THAT(css, HasSubstr(".banner"));
  EXPECT_THAT(css, HasSubstr("div.sponsor > img"));
  // Site rules aren't for every site.
  EXPECT_THAT(css, Not(HasSubstr(".only-a")));
  EXPECT_THAT(css, HasSubstr("{display:none!important}"));
}

TEST_F(CosmeticCssTest, Exceptions) {
  GenericSelectors selectors = CollectGenericSelectors(index_.get());
  EXPECT_TRUE(GetExcludedIndices(index_.get(), "b.test").empty());
  std::vector<uint16_t> excluded = GetExcludedIndices(index_.get(), "www.a.test");
  EXPECT_EQ(excluded.size(), 1u);
  const std::string css = BuildHidingCss(selectors, excluded);
  EXPECT_THAT(css, Not(HasSubstr(".ad-box")));
  EXPECT_THAT(css, HasSubstr(".banner"));
}

TEST(CosmeticCssGroupingTest, SelectorsPerRule) {
  std::vector<std::string> names;
  std::vector<std::string_view> selectors;
  for (size_t i = 0; i < kSelectorsPerRule * 2 + 1; ++i) {
    names.push_back(".s" + std::to_string(i));
  }
  selectors.assign(names.begin(), names.end());
  const std::string css = BuildHidingCss(selectors);
  EXPECT_EQ(base::SplitString(css, "\n", base::TRIM_WHITESPACE,
                              base::SPLIT_WANT_NONEMPTY)
                .size(),
            3u);
  EXPECT_TRUE(BuildHidingCss(std::vector<std::string_view>()).empty());
}

}  // namespace
}  // namespace compass::adblock
