// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/sidebar/link_rules.h"

#include <string>

#include "base/strings/string_util.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"
#include "url/url_util.h"

namespace compass {

namespace {

// |host| (from a GURL: lowercase, punycode) without a trailing dot, or empty
// if it isn't a domain name.
std::string DomainOf(const GURL& url) {
  if (!url.is_valid() || !url.SchemeIsHTTPOrHTTPS() || url.HostIsIPAddress()) {
    return std::string();
  }
  std::string host(url.host());
  if (base::EndsWith(host, ".")) {
    host.pop_back();
  }
  return host;
}

}  // namespace

std::optional<std::string> NormalizeLinkRuleDomain(std::string_view input) {
  std::string text(base::TrimWhitespaceASCII(input, base::TRIM_ALL));
  if (text.empty() || text.find_first_of(" \t\r\n") != std::string::npos) {
    return std::nullopt;
  }
  if (text.find("://") == std::string::npos) {
    // A bare domain, maybe written as a wildcard.
    if (base::StartsWith(text, "*.")) {
      text.erase(0, 2);
    }
    text = "https://" + text;
  }
  const GURL url(text);
  std::string domain = DomainOf(url);
  // Letters, digits, hyphens and dots between non-empty labels (what GURL
  // escapes or keeps of "*" and the like isn't a domain).
  if (domain.empty() || domain.front() == '.' ||
      domain.find("..") != std::string::npos ||
      domain.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-.") !=
          std::string::npos) {
    return std::nullopt;
  }
  if (base::StartsWith(domain, "www.") &&
      domain.find('.', 4) != std::string::npos) {
    domain.erase(0, 4);
  }
  // A domain that can be registered, or one under it: not "com", "co.uk" or
  // "localhost". Unknown suffixes ("router.internal") are fine.
  const GURL domain_url("https://" + domain);
  const std::optional<std::string_view> registry =
      net::registry_controlled_domains::GetRegistry(
          domain_url,
          net::registry_controlled_domains::INCLUDE_UNKNOWN_REGISTRIES,
          net::registry_controlled_domains::EXCLUDE_PRIVATE_REGISTRIES);
  if (!registry || registry->empty() || registry->size() >= domain.size()) {
    return std::nullopt;
  }
  return domain;
}

LinkRules::LinkRules() = default;
LinkRules::LinkRules(const LinkRules&) = default;
LinkRules& LinkRules::operator=(const LinkRules&) = default;
LinkRules::~LinkRules() = default;

bool LinkRules::Set(std::string_view domain, const base::Uuid& space_id) {
  const std::optional<std::string> normalized =
      NormalizeLinkRuleDomain(domain);
  if (!normalized || *normalized != domain || !space_id.is_valid()) {
    return false;
  }
  rules_[*normalized] = space_id;
  return true;
}

bool LinkRules::Remove(std::string_view domain) {
  return rules_.erase(std::string(domain)) > 0;
}

const base::Uuid* LinkRules::Find(std::string_view domain) const {
  auto it = rules_.find(std::string(domain));
  return it == rules_.end() ? nullptr : &it->second;
}

std::optional<LinkRule> LinkRules::Match(const GURL& url) const {
  const std::string domain = DomainOf(url);
  std::string_view host = domain;
  // The host, then each parent domain: the most specific rule first.
  while (!host.empty()) {
    auto it = rules_.find(std::string(host));
    if (it != rules_.end()) {
      return LinkRule{it->first, it->second};
    }
    const size_t dot = host.find('.');
    if (dot == std::string_view::npos) {
      break;
    }
    host.remove_prefix(dot + 1);
  }
  return std::nullopt;
}

std::vector<LinkRule> LinkRules::rules() const {
  std::vector<LinkRule> rules;
  rules.reserve(rules_.size());
  for (const auto& [domain, space_id] : rules_) {
    rules.push_back({domain, space_id});
  }
  return rules;
}

}  // namespace compass
