// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_SIDEBAR_LINK_RULES_H_
#define COMPASS_BROWSER_SIDEBAR_LINK_RULES_H_

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/flat_map.h"
#include "base/uuid.h"

class GURL;

namespace compass {

// Link routing (Arc's Air Traffic Control, docs/design/link-routing.md): a
// link from another app whose domain has a rule opens in the rule's space
// instead of a little window.
struct LinkRule {
  // Normalized (NormalizeLinkRuleDomain()): lowercase ASCII, punycode.
  std::string domain;
  base::Uuid space_id;

  bool operator==(const LinkRule&) const = default;
};

// The domain a rule is for, from what the user typed or a page's URL:
// "GitHub.com", "https://www.github.com/x" and "github.com:443" all give
// "github.com". Lowercase, IDN as punycode ("bücher.de" is
// "xn--bcher-kva.de"), without a leading "www." or "*.", a trailing dot, a
// port or a path. Nullopt for anything that isn't a domain: empty, invalid,
// an IP address, a scheme other than http(s), a single name ("localhost")
// or a bare public suffix ("com", "co.uk").
std::optional<std::string> NormalizeLinkRuleDomain(std::string_view input);

// A profile's rules, one per domain. A rule for a domain also covers its
// subdomains; the most specific rule wins (gist.github.com's over
// github.com's). Rules name spaces by ID and outlive them: a rule whose space
// is gone is ignored when routing, and settings list it as broken.
class LinkRules {
 public:
  LinkRules();
  LinkRules(const LinkRules&);
  LinkRules& operator=(const LinkRules&);
  ~LinkRules();

  // Sets |domain|'s rule (replacing one it had). False, and nothing
  // changes, if |domain| isn't normalized or |space_id| isn't valid.
  bool Set(std::string_view domain, const base::Uuid& space_id);
  // False if |domain| had no rule.
  bool Remove(std::string_view domain);
  // The rule for exactly |domain| (normalized), or null.
  const base::Uuid* Find(std::string_view domain) const;

  // The rule for |url|'s host or its closest parent domain, or nullopt.
  // Only http(s) URLs with a domain (not an IP address) match; ports, paths
  // and the host's case don't matter.
  std::optional<LinkRule> Match(const GURL& url) const;

  // By domain.
  std::vector<LinkRule> rules() const;
  bool empty() const { return rules_.empty(); }
  size_t size() const { return rules_.size(); }

 private:
  base::flat_map<std::string, base::Uuid> rules_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_SIDEBAR_LINK_RULES_H_
