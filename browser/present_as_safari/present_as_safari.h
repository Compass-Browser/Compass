// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_PRESENT_AS_SAFARI_PRESENT_AS_SAFARI_H_
#define COMPASS_BROWSER_PRESENT_AS_SAFARI_PRESENT_AS_SAFARI_H_

#include <optional>
#include <string>
#include <vector>

class GURL;
class PrefService;

namespace content {
class WebContents;
}

// "Present as Safari" (docs/design/fairplay.md): sites the user picked get
// Safari's user agent, without user agent client hints, so that players
// which use FairPlay only in Safari (Bitmovin, Brightcove, JW Player, ...)
// use it here, through AVFoundation. Off for every site by default; macOS
// only, where FairPlay is.
namespace compass::present_as_safari {

// The user agent of Safari 26 on macOS. Safari has frozen the macOS version
// at 10_15_7 since Big Sur.
inline constexpr char kSafariUserAgent[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/26.1 Safari/605.1.15";

// Whether the setting is offered: macOS, with FairPlay on.
bool IsAvailable();

// The site the setting is kept under for `url` (its registrable domain), or
// nullopt for URLs it doesn't apply to (not http(s), IP addresses, ...).
std::optional<std::string> SiteFor(const GURL& url);

bool IsEnabledFor(const PrefService* prefs, const GURL& url);
void SetEnabledFor(PrefService* prefs, const GURL& url, bool enabled);
std::vector<std::string> Sites(const PrefService* prefs);

// Upstream's tab helpers call this
// (patches/chrome-browser-ui-tab_helpers.cc.patch).
void AttachPresentAsSafari(content::WebContents* web_contents);

}  // namespace compass::present_as_safari

#endif  // COMPASS_BROWSER_PRESENT_AS_SAFARI_PRESENT_AS_SAFARI_H_
