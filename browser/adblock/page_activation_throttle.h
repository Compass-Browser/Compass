// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ADBLOCK_PAGE_ACTIVATION_THROTTLE_H_
#define COMPASS_BROWSER_ADBLOCK_PAGE_ACTIVATION_THROTTLE_H_

#include "content/public/browser/navigation_throttle.h"

class GURL;
class HostContentSettingsMap;
class PrefService;

namespace content {
class BrowserContext;
}  // namespace content

namespace compass::adblock {

// Whether the ad blocker blocks on `url`: the blocker is on (`local_state`),
// the page is http(s), and the profile's Ads site setting doesn't allow
// ads there.
bool IsAdBlockActiveFor(const PrefService* local_state,
                        const HostContentSettingsMap* settings,
                        const GURL& url);
// The same for `context`'s profile, with the browser's Local State.
bool IsAdBlockActiveFor(content::BrowserContext* context, const GURL& url);

// Whether the profile's Ads site setting lets `url`'s site show ads (the
// ad blocker's switch for the site).
bool AdsAllowedOn(const HostContentSettingsMap* settings, const GURL& url);
// Sets it: ALLOW for the site, or back to the default (blocked).
void SetAdsAllowedOn(HostContentSettingsMap* settings,
                     const GURL& url,
                     bool allowed);

// Turns upstream's subresource filter on for a page the ad blocker blocks
// on (docs/design/ad-blocker.md "Turning it on"). Upstream's filter runs on
// every page anyway, in dry run (it tags ads, blocking nothing); this tells
// it the page's activation is "enabled" before its own throttle computes
// what applies to the document, and again after each redirect. Pages it
// doesn't block on stay in dry run.
class PageActivationThrottle : public content::NavigationThrottle {
 public:
  explicit PageActivationThrottle(content::NavigationThrottleRegistry& registry);
  PageActivationThrottle(const PageActivationThrottle&) = delete;
  PageActivationThrottle& operator=(const PageActivationThrottle&) = delete;
  ~PageActivationThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  const char* GetNameForLogging() override;

 private:
  void NotifyActivation();

  bool notified_enabled_ = false;
};

}  // namespace compass::adblock

#endif  // COMPASS_BROWSER_ADBLOCK_PAGE_ACTIVATION_THROTTLE_H_
