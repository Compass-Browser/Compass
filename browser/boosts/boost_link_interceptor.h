// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_BOOSTS_BOOST_LINK_INTERCEPTOR_H_
#define COMPASS_BROWSER_BOOSTS_BOOST_LINK_INTERCEPTOR_H_

namespace content {
class WebContents;
}

namespace compass::boosts {

// A tab opening a Boost link (https://compassbrowser.org/boost#…, see
// boost_share.h) gets the Boosts settings section's import preview instead
// of the website: in the same tab if it had nothing loaded yet (a link from
// another app, a new tab), else in a new tab next to it, leaving the page
// where it was. In Incognito, the preview opens in a normal window (the
// settings never open off the record), and adds the Boost to the profile.
// docs/design/boosts.md, "Sharing".
//
// Attaches the helper that does it. Upstream's TabFeatures::Init()
// calls this for every tab (see patches/chrome-browser-ui-tabs-tab_features.cc.patch).
void AttachBoostLinkInterceptor(content::WebContents* web_contents);

}  // namespace compass::boosts

#endif  // COMPASS_BROWSER_BOOSTS_BOOST_LINK_INTERCEPTOR_H_
