// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/link_rules.h"

#include <optional>
#include <string>
#include <vector>

#include "base/uuid.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

TEST(LinkRulesTest, NormalizesDomains) {
  EXPECT_EQ(NormalizeLinkRuleDomain("github.com"), "github.com");
  EXPECT_EQ(NormalizeLinkRuleDomain("  GitHub.COM "), "github.com");
  EXPECT_EQ(NormalizeLinkRuleDomain("www.github.com"), "github.com");
  EXPECT_EQ(NormalizeLinkRuleDomain("*.github.com"), "github.com");
  EXPECT_EQ(NormalizeLinkRuleDomain("github.com."), "github.com");
  EXPECT_EQ(NormalizeLinkRuleDomain("gist.github.com"), "gist.github.com");
  // Ports, paths, queries and schemes are dropped.
  EXPECT_EQ(NormalizeLinkRuleDomain("github.com:8443/a/b?c=d"), "github.com");
  EXPECT_EQ(NormalizeLinkRuleDomain("https://www.github.com/compass#x"),
            "github.com");
  EXPECT_EQ(NormalizeLinkRuleDomain("http://docs.example.co.uk:80/"),
            "docs.example.co.uk");
  // IDN goes to punycode, whichever way it's written.
  EXPECT_EQ(NormalizeLinkRuleDomain("bücher.de"), "xn--bcher-kva.de");
  EXPECT_EQ(NormalizeLinkRuleDomain("BÜCHER.de"), "xn--bcher-kva.de");
  EXPECT_EQ(NormalizeLinkRuleDomain("xn--bcher-kva.de"), "xn--bcher-kva.de");
  // Unknown suffixes and private registries are fine.
  EXPECT_EQ(NormalizeLinkRuleDomain("router.internal"), "router.internal");
  EXPECT_EQ(NormalizeLinkRuleDomain("github.io"), "github.io");
  // "www." stays when it's all there is.
  EXPECT_EQ(NormalizeLinkRuleDomain("www.com"), "www.com");
}

TEST(LinkRulesTest, RejectsWhatIsNotADomain) {
  for (const char* input :
       {"", "   ", "github .com", "http://", "https:///path", "ftp://github.com",
        "file:///etc/hosts", "javascript:alert(1)", "203.0.113.7",
        "http://[::1]/", "localhost", "com", "co.uk", "*.com", "foo..com",
        ".github.com", "*", "*.*.github.com", "git*hub.com",
        "chrome://settings"}) {
    EXPECT_EQ(NormalizeLinkRuleDomain(input), std::nullopt) << input;
  }
}

TEST(LinkRulesTest, SetTakesNormalizedDomainsAndValidSpaces) {
  LinkRules rules;
  const base::Uuid space = base::Uuid::GenerateRandomV4();
  EXPECT_FALSE(rules.Set("GitHub.com", space));
  EXPECT_FALSE(rules.Set("github.com", base::Uuid()));
  EXPECT_FALSE(rules.Set("localhost", space));
  EXPECT_TRUE(rules.empty());
  EXPECT_TRUE(rules.Set("github.com", space));
  ASSERT_TRUE(rules.Find("github.com"));
  EXPECT_EQ(*rules.Find("github.com"), space);
  // One rule per domain.
  const base::Uuid other = base::Uuid::GenerateRandomV4();
  EXPECT_TRUE(rules.Set("github.com", other));
  EXPECT_EQ(rules.size(), 1u);
  EXPECT_EQ(*rules.Find("github.com"), other);
  EXPECT_TRUE(rules.Remove("github.com"));
  EXPECT_FALSE(rules.Remove("github.com"));
  EXPECT_TRUE(rules.empty());
}

TEST(LinkRulesTest, MatchesSubdomainsMostSpecificFirst) {
  LinkRules rules;
  const base::Uuid work = base::Uuid::GenerateRandomV4();
  const base::Uuid notes = base::Uuid::GenerateRandomV4();
  ASSERT_TRUE(rules.Set("github.com", work));
  ASSERT_TRUE(rules.Set("gist.github.com", notes));
  auto space_of = [&](const char* url) -> std::optional<base::Uuid> {
    std::optional<LinkRule> rule = rules.Match(GURL(url));
    return rule ? std::optional<base::Uuid>(rule->space_id) : std::nullopt;
  };
  EXPECT_EQ(space_of("https://github.com/"), work);
  EXPECT_EQ(space_of("https://www.github.com/compass"), work);
  EXPECT_EQ(space_of("https://api.eu.github.com/"), work);
  EXPECT_EQ(space_of("https://gist.github.com/x"), notes);
  EXPECT_EQ(space_of("https://raw.gist.github.com/x"), notes);
  EXPECT_EQ(rules.Match(GURL("https://gist.github.com/"))->domain,
            "gist.github.com");
  // Not a subdomain, only a similar name.
  EXPECT_EQ(space_of("https://notgithub.com/"), std::nullopt);
  EXPECT_EQ(space_of("https://github.com.evil.example/"), std::nullopt);
  EXPECT_EQ(space_of("https://github.co/"), std::nullopt);
}

TEST(LinkRulesTest, MatchIgnoresCasePortsAndPaths) {
  LinkRules rules;
  const base::Uuid space = base::Uuid::GenerateRandomV4();
  ASSERT_TRUE(rules.Set("github.com", space));
  for (const char* url :
       {"https://GITHUB.com/", "http://github.com:8080/a/b?c#d",
        "https://github.com./", "HTTPS://Gist.GitHub.Com/x"}) {
    std::optional<LinkRule> rule = rules.Match(GURL(url));
    ASSERT_TRUE(rule) << url;
    EXPECT_EQ(rule->space_id, space) << url;
  }
}

TEST(LinkRulesTest, MatchesIdn) {
  LinkRules rules;
  const base::Uuid space = base::Uuid::GenerateRandomV4();
  ASSERT_TRUE(rules.Set(*NormalizeLinkRuleDomain("bücher.de"), space));
  EXPECT_TRUE(rules.Match(GURL("https://bücher.de/")));
  EXPECT_TRUE(rules.Match(GURL("https://shop.xn--bcher-kva.de/")));
  EXPECT_FALSE(rules.Match(GURL("https://bucher.de/")));
}

TEST(LinkRulesTest, OnlyWebPagesMatch) {
  LinkRules rules;
  ASSERT_TRUE(rules.Set("example.com", base::Uuid::GenerateRandomV4()));
  for (const char* url :
       {"file:///example.com", "ftp://example.com/", "chrome://example.com/",
        "mailto:me@example.com", "not a url", "https://198.51.100.4/"}) {
    EXPECT_FALSE(rules.Match(GURL(url))) << url;
  }
}

TEST(LinkRulesTest, SpaceListRoutesToExistingSpaces) {
  SpaceList list;
  SpaceModel* work = list.AddSpace(1, u"Work");
  const base::Uuid work_id = work->id();
  ASSERT_TRUE(list.SetLinkRule("github.com", work_id));
  // Setting it again changes nothing.
  EXPECT_FALSE(list.SetLinkRule("github.com", work_id));
  EXPECT_EQ(list.FindRoutedSpace(GURL("https://github.com/")), work);
  EXPECT_FALSE(list.FindRoutedSpace(GURL("https://example.com/")));
  // Its space deleted: the rule stays, and routes nothing.
  list.RemoveSpace(*work);
  ASSERT_TRUE(list.link_rules().Find("github.com"));
  EXPECT_FALSE(list.FindRoutedSpace(GURL("https://github.com/")));
  EXPECT_TRUE(list.RemoveLinkRule("github.com"));
  EXPECT_TRUE(list.link_rules().empty());
}

class RulesRecorder : public SpaceList::Observer {
 public:
  void OnLinkRulesChanged() override { ++changes; }
  int changes = 0;
};

TEST(LinkRulesTest, SpaceListTellsObservers) {
  SpaceList list;
  RulesRecorder recorder;
  list.AddObserver(&recorder);
  const base::Uuid home = list.spaces()[0]->id();
  EXPECT_TRUE(list.SetLinkRule("github.com", home));
  EXPECT_FALSE(list.SetLinkRule("github.com", home));
  EXPECT_FALSE(list.SetLinkRule("Not a domain", home));
  EXPECT_TRUE(list.RemoveLinkRule("github.com"));
  EXPECT_FALSE(list.RemoveLinkRule("github.com"));
  EXPECT_EQ(recorder.changes, 2);
  list.RemoveObserver(&recorder);
}

}  // namespace
}  // namespace compass
