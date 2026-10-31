// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_COMPASS_URL_HANDLER_H_
#define COMPASS_BROWSER_COMPASS_URL_HANDLER_H_

namespace content {
class BrowserURLHandler;
}  // namespace content

namespace compass {

// Makes compass://x load chrome://x. Added first by
// ChromeContentBrowserClient::BrowserURLHandlerCreated() (see patches/), so
// upstream's own rewrites (chrome://newtab, chrome://about, WebUI) still see
// the chrome:// URL. Once the page commits, the tab's shown (virtual) URL is
// the chrome:// one, so upstream code that checks it keeps working; display
// code writes it as compass:// (ToCompassURL(), ToCompassText()).
void AddCompassURLHandler(content::BrowserURLHandler* handler);

}  // namespace compass

#endif  // COMPASS_BROWSER_COMPASS_URL_HANDLER_H_
