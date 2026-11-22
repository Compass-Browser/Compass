// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_ADBLOCK_ADBLOCK_HOOKS_H_
#define COMPASS_BROWSER_ADBLOCK_ADBLOCK_HOOKS_H_

namespace content {
class NavigationThrottleRegistry;
}  // namespace content

// What upstream's code calls into the ad blocker with
// (docs/design/ad-blocker.md). Declarations only, so the targets that call
// them (//chrome/browser:core) don't depend on the ad blocker's own deps;
// //compass/browser/adblock, linked into //chrome/browser, defines them.
namespace compass::adblock {

// After the browser starts (patches/chrome-browser-chrome_browser_main.cc
// .patch): creates the service, supplies the ruleset, schedules the list
// downloads.
void StartAdBlock();

// For every navigation, before upstream's subresource filter throttles
// (patches/chrome-browser-chrome_content_browser_client_navigation_throttles
// .cc.patch).
void MaybeAddPageActivationThrottle(
    content::NavigationThrottleRegistry& registry);

}  // namespace compass::adblock

#endif  // COMPASS_BROWSER_ADBLOCK_ADBLOCK_HOOKS_H_
