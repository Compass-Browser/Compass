// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_WEB_STORE_WEB_STORE_BRANDING_H_
#define COMPASS_BROWSER_WEB_STORE_WEB_STORE_BRANDING_H_

#include <string>

namespace content {
class WebContents;
}

class GURL;

namespace compass {

// The Chrome Web Store's buttons name the browser ("Add to Chrome", "Remove
// from Chrome", in the page's language). In Compass they say Compass: once a
// store page's DOM is loaded, a script in an isolated world renames
// "Chrome" in the text and labels of its buttons, and keeps doing so as the
// page changes them. docs/design/branding.md "Web Store".

// Attaches the helper that does it. Upstream's TabHelpers::AttachTabHelpers()
// calls this for every tab (see patches/chrome-browser-ui-tab_helpers.cc.patch).
void AttachWebStoreBranding(content::WebContents* web_contents);

// Whether |url| is a Chrome Web Store page.
bool IsWebStoreURL(const GURL& url);

// The script, for tests.
std::u16string WebStoreBrandingScriptForTesting();

}  // namespace compass

#endif  // COMPASS_BROWSER_WEB_STORE_WEB_STORE_BRANDING_H_
