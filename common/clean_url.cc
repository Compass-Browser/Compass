// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/common/clean_url.h"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/no_destructor.h"
#include "base/strings/escape.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "compass/common/strip_on_share_lists.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "url/gurl.h"

namespace compass {

namespace {

using Names = std::set<std::string, std::less<>>;

// Both of Firefox's lists, merged: names in lower case.
struct StripRules {
  Names global;
  std::map<std::string, Names, std::less<>> by_host;
  std::map<std::string, Names, std::less<>> by_site;
};

void AddList(std::string_view json, StripRules& rules) {
  std::optional<base::DictValue> list =
      base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  CHECK(list) << "strip-on-share list isn't a JSON object";
  for (const auto [key, value] : *list) {
    const base::DictValue* rule = value.GetIfDict();
    if (!rule) {
      continue;
    }
    Names names;
    if (const base::ListValue* params = rule->FindList("queryParams")) {
      for (const base::Value& param : *params) {
        if (param.is_string()) {
          names.insert(base::ToLowerASCII(param.GetString()));
        }
      }
    }
    const auto add_to = [&names](Names& to) {
      to.insert(names.begin(), names.end());
    };
    if (rule->FindBool("isGlobal").value_or(false)) {
      add_to(rules.global);
    }
    for (const auto [field, map] :
         {std::pair{"hosts", &rules.by_host},
          std::pair{"schemelessSites", &rules.by_site}}) {
      if (const base::ListValue* hosts = rule->FindList(field)) {
        for (const base::Value& host : *hosts) {
          if (host.is_string()) {
            add_to((*map)[base::ToLowerASCII(host.GetString())]);
          }
        }
      }
    }
  }
}

const StripRules& Rules() {
  static const base::NoDestructor<StripRules> rules([] {
    StripRules rules;
    AddList(strip_on_share::kMpl2List, rules);
    AddList(strip_on_share::kLgplList, rules);
    return rules;
  }());
  return *rules;
}

const Names* Find(const std::map<std::string, Names, std::less<>>& map,
                  std::string_view key) {
  auto it = map.find(key);
  return it == map.end() ? nullptr : &it->second;
}

// Cleans |url|'s query; |nested| also cleans URLs in parameter values.
// Returns whether it removed anything.
bool Clean(const GURL& url, bool nested, GURL& out) {
  if (!url.SchemeIsHTTPOrHTTPS() || !url.has_query()) {
    return false;
  }
  const StripRules& rules = Rules();
  const std::string_view host = url.host();
  std::string site = net::registry_controlled_domains::GetDomainAndRegistry(
      url, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  if (site.empty()) {
    site = std::string(host);  // An IP address or a bare host, as Firefox.
  }
  const Names* host_names = Find(rules.by_host, host);
  const Names* site_names = Find(rules.by_site, site);

  std::vector<std::string> kept;
  bool removed = false;
  for (std::string_view part :
       base::SplitStringPiece(url.query(), "&", base::KEEP_WHITESPACE,
                              base::SPLIT_WANT_ALL)) {
    const size_t equals = part.find('=');
    // Compared as written, not unescaped: the lists spell out the escaped
    // names they want ("__cft__%5B0%5D").
    const std::string name = base::ToLowerASCII(part.substr(0, equals));
    if (!name.empty() &&
        (rules.global.contains(name) ||
         (host_names && host_names->contains(name)) ||
         (site_names && site_names->contains(name)))) {
      removed = true;
      continue;
    }
    if (nested && equals != std::string_view::npos) {
      const std::string value = base::UnescapeBinaryURLComponent(
          part.substr(equals + 1), base::UnescapeRule::REPLACE_PLUS_WITH_SPACE);
      GURL inner;
      if (Clean(GURL(value), /*nested=*/false, inner)) {
        removed = true;
        kept.push_back(std::string(part.substr(0, equals + 1)) +
                       base::EscapeQueryParamValue(inner.spec(),
                                                   /*use_plus=*/true));
        continue;
      }
    }
    kept.emplace_back(part);
  }
  if (!removed) {
    return false;
  }
  GURL::Replacements replacements;
  const std::string query = base::JoinString(kept, "&");
  if (query.empty()) {
    replacements.ClearQuery();
  } else {
    replacements.SetQueryStr(query);
  }
  out = url.ReplaceComponents(replacements);
  return true;
}

}  // namespace

GURL RemoveTrackingParams(const GURL& url) {
  GURL cleaned;
  return Clean(url, /*nested=*/true, cleaned) ? cleaned : url;
}

}  // namespace compass
