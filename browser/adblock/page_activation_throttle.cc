// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/adblock/page_activation_throttle.h"

#include <memory>

#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/adblock/adblock_hooks.h"
#include "compass/browser/adblock/adblock_prefs.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/prefs/pref_service.h"
#include "components/subresource_filter/content/browser/subresource_filter_observer_manager.h"
#include "components/subresource_filter/content/browser/utils.h"
#include "components/subresource_filter/core/mojom/subresource_filter.mojom.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/navigation_throttle_registry.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace compass::adblock {

bool AdsAllowedOn(const HostContentSettingsMap* settings, const GURL& url) {
  return settings &&
         settings->GetContentSetting(url, GURL(), ContentSettingsType::ADS) ==
             CONTENT_SETTING_ALLOW;
}

void SetAdsAllowedOn(HostContentSettingsMap* settings,
                     const GURL& url,
                     bool allowed) {
  if (!settings || !url.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  settings->SetContentSettingDefaultScope(
      url, GURL(), ContentSettingsType::ADS,
      allowed ? CONTENT_SETTING_ALLOW : CONTENT_SETTING_DEFAULT);
}

bool IsAdBlockActiveFor(const PrefService* local_state,
                        const HostContentSettingsMap* settings,
                        const GURL& url) {
  return local_state && local_state->GetBoolean(prefs::kEnabled) &&
         url.SchemeIsHTTPOrHTTPS() && !AdsAllowedOn(settings, url);
}

bool IsAdBlockActiveFor(content::BrowserContext* context, const GURL& url) {
  if (!g_browser_process || !context) {
    return false;
  }
  return IsAdBlockActiveFor(
      g_browser_process->local_state(),
      HostContentSettingsMapFactory::GetForProfile(
          Profile::FromBrowserContext(context)),
      url);
}

PageActivationThrottle::PageActivationThrottle(
    content::NavigationThrottleRegistry& registry)
    : content::NavigationThrottle(registry) {}

PageActivationThrottle::~PageActivationThrottle() = default;

content::NavigationThrottle::ThrottleCheckResult
PageActivationThrottle::WillStartRequest() {
  NotifyActivation();
  return PROCEED;
}

content::NavigationThrottle::ThrottleCheckResult
PageActivationThrottle::WillRedirectRequest() {
  NotifyActivation();
  return PROCEED;
}

const char* PageActivationThrottle::GetNameForLogging() {
  return "CompassAdBlockPageActivationThrottle";
}

void PageActivationThrottle::NotifyActivation() {
  content::NavigationHandle* handle = navigation_handle();
  const bool block = IsAdBlockActiveFor(
      handle->GetWebContents()->GetBrowserContext(), handle->GetURL());
  // Only a change is news: a redirect from a site it blocks on to one it
  // doesn't goes back to upstream's dry run, and the other way enables it.
  // Telling the observers "enabled" again for each redirect between sites it
  // blocks on (a sign-in's chain of them) trips the ads metrics observer,
  // which takes one activation per page
  // (AdsPageLoadMetricsObserver::OnPageActivationComputed's DCHECK).
  if (block == notified_enabled_) {
    return;
  }
  auto* observer_manager =
      subresource_filter::SubresourceFilterObserverManager::FromWebContents(
          handle->GetWebContents());
  if (!observer_manager) {
    return;
  }
  subresource_filter::mojom::ActivationState state;
  state.activation_level =
      block ? subresource_filter::mojom::ActivationLevel::kEnabled
            : subresource_filter::mojom::ActivationLevel::kDryRun;
  notified_enabled_ = block;
  observer_manager->NotifyPageActivationComputed(handle, state);
}

void MaybeAddPageActivationThrottle(
    content::NavigationThrottleRegistry& registry) {
  content::NavigationHandle& handle = registry.GetNavigationHandle();
  if (!subresource_filter::IsInSubresourceFilterRoot(&handle) ||
      !subresource_filter::SubresourceFilterObserverManager::FromWebContents(
          handle.GetWebContents())) {
    return;
  }
  registry.AddThrottle(std::make_unique<PageActivationThrottle>(registry));
}

}  // namespace compass::adblock
