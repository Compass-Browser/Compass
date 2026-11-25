// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/present_as_safari/present_as_safari.h"

#include <algorithm>

#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/media/avf/common/avf_features.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "net/base/registry_controlled_domains/registry_controlled_domain.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "url/gurl.h"

namespace compass::present_as_safari {
namespace {

// Gives main-frame navigations to the user's sites Safari's user agent, and
// takes it back for other sites.
class PresentAsSafariTabHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<PresentAsSafariTabHelper> {
 public:
  PresentAsSafariTabHelper(const PresentAsSafariTabHelper&) = delete;
  PresentAsSafariTabHelper& operator=(const PresentAsSafariTabHelper&) =
      delete;
  ~PresentAsSafariTabHelper() override = default;

  // content::WebContentsObserver:
  void DidStartNavigation(content::NavigationHandle* handle) override {
    if (!handle->IsInPrimaryMainFrame() || handle->IsSameDocument()) {
      return;
    }
    content::WebContents* contents = web_contents();
    const PrefService* prefs =
        Profile::FromBrowserContext(contents->GetBrowserContext())->GetPrefs();
    if (IsAvailable() && IsEnabledFor(prefs, handle->GetURL())) {
      // No client hints: Safari sends none, and navigator.userAgentData is
      // null there.
      contents->SetUserAgentOverride(
          blink::UserAgentOverride::UserAgentOnly(kSafariUserAgent),
          /*override_in_new_tabs=*/false);
      handle->SetIsOverridingUserAgent(true);
      return;
    }
    // Leave other overrides (DevTools, other features) alone.
    if (contents->GetUserAgentOverride().ua_string_override ==
        kSafariUserAgent) {
      handle->SetIsOverridingUserAgent(false);
    }
  }

 private:
  explicit PresentAsSafariTabHelper(content::WebContents* contents)
      : content::WebContentsObserver(contents),
        content::WebContentsUserData<PresentAsSafariTabHelper>(*contents) {}
  friend class content::WebContentsUserData<PresentAsSafariTabHelper>;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

WEB_CONTENTS_USER_DATA_KEY_IMPL(PresentAsSafariTabHelper);

}  // namespace

bool IsAvailable() {
#if BUILDFLAG(IS_MAC)
  return avf::IsFairPlayEnabled();
#else
  return false;
#endif
}

std::optional<std::string> SiteFor(const GURL& url) {
  if (!url.SchemeIsHTTPOrHTTPS() || url.HostIsIPAddress()) {
    return std::nullopt;
  }
  std::string site = net::registry_controlled_domains::GetDomainAndRegistry(
      url, net::registry_controlled_domains::INCLUDE_PRIVATE_REGISTRIES);
  if (site.empty()) {
    return std::nullopt;
  }
  return site;
}

bool IsEnabledFor(const PrefService* prefs, const GURL& url) {
  std::optional<std::string> site = SiteFor(url);
  if (!site) {
    return false;
  }
  const base::ListValue& sites = prefs->GetList(prefs::kPresentAsSafariSites);
  return std::ranges::any_of(sites, [&](const base::Value& value) {
    return value.is_string() && value.GetString() == *site;
  });
}

void SetEnabledFor(PrefService* prefs, const GURL& url, bool enabled) {
  std::optional<std::string> site = SiteFor(url);
  if (!site || IsEnabledFor(prefs, url) == enabled) {
    return;
  }
  ScopedListPrefUpdate update(prefs, prefs::kPresentAsSafariSites);
  if (enabled) {
    update->Append(*site);
  } else {
    update->EraseValue(base::Value(*site));
  }
}

std::vector<std::string> Sites(const PrefService* prefs) {
  std::vector<std::string> sites;
  for (const base::Value& value :
       prefs->GetList(prefs::kPresentAsSafariSites)) {
    if (value.is_string()) {
      sites.push_back(value.GetString());
    }
  }
  return sites;
}

void AttachPresentAsSafari(content::WebContents* web_contents) {
  PresentAsSafariTabHelper::CreateForWebContents(web_contents);
}

}  // namespace compass::present_as_safari
