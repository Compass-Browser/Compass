// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/dev_mode/dev_mode.h"

#include <algorithm>

#include "base/strings/strcat.h"
#include "base/strings/string_util.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/boosts/boost_store.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"
#include "url/url_constants.h"

namespace compass::dev_mode {

namespace {

using boosts::BoostMatch;

// The patterns that parse; ones that don't (a hand-edited Preferences file)
// are skipped, not dropped.
std::vector<BoostMatch> Matches(const PrefService* prefs) {
  std::vector<BoostMatch> matches;
  for (const base::Value& value : prefs->GetList(prefs::kDevModeHosts)) {
    if (!value.is_string()) {
      continue;
    }
    base::expected<BoostMatch, std::string> match =
        BoostMatch::Parse(value.GetString());
    if (match.has_value()) {
      matches.push_back(std::move(match.value()));
    }
  }
  return matches;
}

}  // namespace

std::optional<std::string> NormalizeHost(std::string_view input) {
  std::string_view rest = base::TrimWhitespaceASCII(input, base::TRIM_ALL);
  bool subdomains = false;
  if (base::StartsWith(rest, "*.")) {
    subdomains = true;
    rest.remove_prefix(2);
  }
  // A star only as "*." in front: GURL would take "*" as a host.
  if (rest.empty() || rest.find('*') != std::string_view::npos) {
    return std::nullopt;
  }
  // GURL does the host's parsing (case, IDN, ports, IP addresses).
  const GURL url(rest.find("://") == std::string_view::npos
                     ? base::StrCat({url::kHttpScheme, "://", rest})
                     : std::string(rest));
  if (!CanEnable(url) || url.host().empty()) {
    return std::nullopt;
  }
  base::expected<BoostMatch, std::string> match = BoostMatch::Parse(
      base::StrCat({subdomains ? "*." : "", url.host()}));
  if (!match.has_value()) {
    return std::nullopt;
  }
  return match->ToString();
}

bool CanEnable(const GURL& url) {
  return url.is_valid() && url.SchemeIsHTTPOrHTTPS();
}

bool IsEnabledFor(const PrefService* prefs, const GURL& url) {
  if (!prefs || !CanEnable(url)) {
    return false;
  }
  return std::ranges::any_of(
      Matches(prefs), [&url](const BoostMatch& m) { return m.Matches(url); });
}

void SetEnabledFor(PrefService* prefs, const GURL& url, bool enabled) {
  if (!CanEnable(url)) {
    return;
  }
  if (enabled) {
    if (!IsEnabledFor(prefs, url)) {
      AddHost(prefs, url.host());
    }
    return;
  }
  ScopedListPrefUpdate update(prefs, prefs::kDevModeHosts);
  update->EraseIf([&url](const base::Value& value) {
    if (!value.is_string()) {
      return false;
    }
    base::expected<BoostMatch, std::string> match =
        BoostMatch::Parse(value.GetString());
    return match.has_value() && match->Matches(url);
  });
}

std::vector<std::string> Hosts(const PrefService* prefs) {
  std::vector<std::string> hosts;
  for (const base::Value& value : prefs->GetList(prefs::kDevModeHosts)) {
    if (value.is_string()) {
      hosts.push_back(value.GetString());
    }
  }
  return hosts;
}

std::optional<std::string> AddHost(PrefService* prefs,
                                   std::string_view input) {
  std::optional<std::string> pattern = NormalizeHost(input);
  if (!pattern) {
    return std::nullopt;
  }
  ScopedListPrefUpdate update(prefs, prefs::kDevModeHosts);
  if (!std::ranges::contains(*update, base::Value(*pattern))) {
    update->Append(*pattern);
  }
  return pattern;
}

void RemoveHost(PrefService* prefs, std::string_view pattern) {
  ScopedListPrefUpdate update(prefs, prefs::kDevModeHosts);
  update->EraseValue(base::Value(pattern));
}

bool IsEnabledFor(content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  return IsEnabledFor(
      Profile::FromBrowserContext(contents->GetBrowserContext())->GetPrefs(),
      contents->GetLastCommittedURL());
}

bool ShouldBypassCache(content::WebContents* contents) {
  return IsEnabledFor(contents);
}

}  // namespace compass::dev_mode
