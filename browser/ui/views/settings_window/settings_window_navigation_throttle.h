// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_SETTINGS_WINDOW_SETTINGS_WINDOW_NAVIGATION_THROTTLE_H_
#define COMPASS_BROWSER_UI_VIEWS_SETTINGS_WINDOW_SETTINGS_WINDOW_NAVIGATION_THROTTLE_H_

#include "content/public/browser/navigation_throttle.h"

namespace compass {

// A tab loading settings the window has (chrome://settings/,
// chrome://settings/boosts, chrome://compass-settings/...: typed, a link, a
// reload) opens the settings window there instead. The navigation is
// dropped, and a tab that had nothing else (a new one) closes. Upstream's
// sections the window doesn't have yet (chrome://settings/searchEngines)
// load as before. Added by CreateAndAddChromeThrottlesForNavigation()
// (see patches/). See docs/design/settings-window.md.
class SettingsWindowNavigationThrottle : public content::NavigationThrottle {
 public:
  static void MaybeCreateAndAdd(content::NavigationThrottleRegistry& registry);

  explicit SettingsWindowNavigationThrottle(
      content::NavigationThrottleRegistry& registry);
  ~SettingsWindowNavigationThrottle() override;

  // content::NavigationThrottle:
  ThrottleCheckResult WillStartRequest() override;
  const char* GetNameForLogging() override;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_SETTINGS_WINDOW_SETTINGS_WINDOW_NAVIGATION_THROTTLE_H_
