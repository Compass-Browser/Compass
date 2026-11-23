// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_ZAP_ZAP_SESSION_H_
#define COMPASS_BROWSER_UI_ZAP_ZAP_SESSION_H_

#include <optional>
#include <string>

namespace content {
class WebContents;
}  // namespace content

// Zap (docs/design/boosts.md, "Zap"): click elements of a page away; Done
// keeps them hidden on the site with its zap Boost.
namespace compass::zap {

// The site |contents|' page would be zapped on (a BoostMatch pattern for
// its host), or none: not a web page, or a profile without Boosts.
std::optional<std::string> ZapSiteFor(content::WebContents* contents);

// Puts Zap's bar on |contents|' page; again while it's up starts over.
void StartZap(content::WebContents* contents);

}  // namespace compass::zap

#endif  // COMPASS_BROWSER_UI_ZAP_ZAP_SESSION_H_
