// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/ruleset_builder.h"

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "compass/browser/adblock/filter_lists.h"
#include "components/subresource_filter/core/common/first_party_origin.h"
#include "components/subresource_filter/core/common/indexed_ruleset.h"
#include "components/subresource_filter/core/common/load_policy.h"
#include "components/subresource_filter/core/common/unindexed_ruleset.h"
#include "components/url_pattern_index/proto/rules.pb.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/protobuf/src/google/protobuf/io/zero_copy_stream_impl_lite.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace compass::adblock {
namespace {

namespace proto = url_pattern_index::proto;
using subresource_filter::LoadPolicy;
using ::testing::Contains;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

// Indexes a built ruleset as upstream's ruleset service does.
class Indexed {
 public:
  explicit Indexed(const std::string& unindexed) : indexer_(/*ruleset_id=*/1) {
    google::protobuf::io::ArrayInputStream stream(unindexed.data(),
                                                  unindexed.size());
    subresource_filter::UnindexedRulesetReader reader(&stream);
    proto::FilteringRules chunk;
    while (reader.ReadNextChunk(&chunk)) {
      for (const proto::UrlRule& rule : chunk.url_rules()) {
        indexer_.AddUrlRule(rule);
      }
      for (const proto::StyleRule& rule : chunk.style_rules()) {
        indexer_.AddStyleRuleFromProto(rule);
      }
    }
    indexer_.Finish();
    matcher_ = std::make_unique<subresource_filter::IndexedRulesetMatcher>(
        indexer_.data());
  }

  LoadPolicy Load(const std::string& url,
                  const std::string& page,
                  proto::ElementType type = proto::ELEMENT_TYPE_SCRIPT) {
    return matcher_->GetLoadPolicyForResourceLoad(
        GURL(url),
        subresource_filter::FirstPartyOrigin(url::Origin::Create(GURL(page))),
        type, /*disable_generic_rules=*/false, nullptr);
  }

  bool DocumentAllowed(const std::string& page) {
    return matcher_->ShouldDisableFilteringForDocument(
        GURL(page), url::Origin(), proto::ACTIVATION_TYPE_DOCUMENT);
  }

  std::vector<std::string> DomainSelectors(const std::string& page) {
    std::vector<std::string_view> selectors;
    matcher_->GetDomainSelectors(url::Origin::Create(GURL(page)), selectors);
    return {selectors.begin(), selectors.end()};
  }

 private:
  subresource_filter::RulesetIndexer indexer_;
  std::unique_ptr<subresource_filter::IndexedRulesetMatcher> matcher_;
};

TEST(RulesetBuilderTest, NetworkRules) {
  std::map<std::string, ListCounts> counts;
  Indexed indexed(BuildUnindexedRuleset(
      {{"list",
        "[Adblock Plus 2.0]\n"
        "! Title: Test\n"
        "||ads.test^\n"
        "||tracker.test^$3p\n"
        "/banner/*$image\n"
        "@@||ads.test/ok.js\n"
        "@@||allowed.test^$document\n"
        "||x.test^$redirect=noopjs\n"}},
      &counts));

  EXPECT_EQ(indexed.Load("https://ads.test/ad.js", "https://a.test/"),
            LoadPolicy::DISALLOW);
  EXPECT_EQ(indexed.Load("https://ads.test/ok.js", "https://a.test/"),
            LoadPolicy::EXPLICITLY_ALLOW);
  EXPECT_EQ(indexed.Load("https://tracker.test/t.js", "https://a.test/"),
            LoadPolicy::DISALLOW);
  EXPECT_EQ(indexed.Load("https://tracker.test/t.js", "https://tracker.test/"),
            LoadPolicy::ALLOW);
  EXPECT_EQ(indexed.Load("https://cdn.test/banner/1.png", "https://a.test/",
                         proto::ELEMENT_TYPE_IMAGE),
            LoadPolicy::DISALLOW);
  EXPECT_EQ(indexed.Load("https://cdn.test/banner/1.js", "https://a.test/"),
            LoadPolicy::ALLOW);
  EXPECT_EQ(indexed.Load("https://cdn.test/app.js", "https://a.test/"),
            LoadPolicy::ALLOW);
  EXPECT_TRUE(indexed.DocumentAllowed("https://allowed.test/"));
  EXPECT_FALSE(indexed.DocumentAllowed("https://a.test/"));

  EXPECT_EQ(counts["list"], (ListCounts{.url_rules = 5, .skipped = 1}));
}

TEST(RulesetBuilderTest, ElementHidingRules) {
  std::map<std::string, ListCounts> counts;
  Indexed indexed(BuildUnindexedRuleset(
      {{"list",
        "##.ad-box\n"
        "a.test##.only-a\n"
        "a.test#@#.ad-box\n"
        "b.test,~sub.b.test##.only-b\n"
        "a.test#?#div:has-text(Ad)\n"}},
      &counts));

  EXPECT_THAT(indexed.DomainSelectors("https://a.test/"),
              UnorderedElementsAre(".only-a"));
  EXPECT_THAT(indexed.DomainSelectors("https://www.a.test/"),
              UnorderedElementsAre(".only-a"));
  EXPECT_THAT(indexed.DomainSelectors("https://b.test/"),
              UnorderedElementsAre(".only-b"));
  EXPECT_THAT(indexed.DomainSelectors("https://sub.b.test/"),
              Not(Contains(".only-b")));
  EXPECT_EQ(counts["list"], (ListCounts{.css_rules = 4, .skipped = 1}));
}

TEST(RulesetBuilderTest, SharedRulesOnce) {
  const std::string one = BuildUnindexedRuleset(
      {{"a", "||ads.test^\n##.ad-box\n"}}, nullptr);
  const std::string twice = BuildUnindexedRuleset(
      {{"a", "||ads.test^\n##.ad-box\n"}, {"b", "||ads.test^\n##.ad-box\n"}},
      nullptr);
  EXPECT_EQ(one, twice);
}

TEST(RulesetBuilderTest, ContentVersion) {
  const std::string version = ComputeContentVersion({{"a", "||x^"}});
  EXPECT_EQ(version, ComputeContentVersion({{"a", "||x^"}}));
  EXPECT_NE(version, ComputeContentVersion({{"a", "||y^"}}));
  EXPECT_NE(version, ComputeContentVersion({{"b", "||x^"}}));
  EXPECT_NE(version, ComputeContentVersion({{"a", "||x^"}, {"b", "||y^"}}));
}

TEST(RulesetBuilderTest, BuildsFileOnceAndRemovesOldOnes) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath list = GetListTextPath(dir.GetPath(), "a");
  ASSERT_TRUE(base::CreateDirectory(list.DirName()));
  ASSERT_TRUE(base::WriteFile(list, "||ads.test^\n"));

  std::optional<BuiltRuleset> first = BuildRulesetFile(dir.GetPath(), {"a"});
  ASSERT_TRUE(first);
  EXPECT_TRUE(base::PathExists(first->path));
  ASSERT_TRUE(first->counts);
  EXPECT_EQ((*first->counts)["a"].url_rules, 1);

  // The same lists: found, not built.
  std::optional<BuiltRuleset> again = BuildRulesetFile(dir.GetPath(), {"a"});
  ASSERT_TRUE(again);
  EXPECT_EQ(again->content_version, first->content_version);
  EXPECT_FALSE(again->counts);

  // No list: an empty ruleset, and the older one is gone.
  std::optional<BuiltRuleset> none = BuildRulesetFile(dir.GetPath(), {});
  ASSERT_TRUE(none);
  EXPECT_NE(none->content_version, first->content_version);
  EXPECT_FALSE(base::PathExists(first->path));
}

}  // namespace
}  // namespace compass::adblock
