// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/compass_url_handler.h"

#include "compass/common/compass_url.h"
#include "content/public/browser/browser_url_handler.h"
#include "url/gurl.h"

namespace compass {

namespace {

// compass://x → chrome://x, then the rest of the handlers on chrome://x (a
// handler returning true ends the chain, and returning false would leave the
// navigation entry's virtual URL as compass:// for good). Returning true
// with a reverse handler makes the entry take the committed URL as its
// virtual URL.
bool RewriteCompassURL(GURL* url, content::BrowserContext* browser_context) {
  if (!url->SchemeIs(kCompassScheme)) {
    return false;
  }
  *url = ToChromeURL(*url);
  // chrome:// never matches this handler, so this doesn't come back here.
  content::BrowserURLHandler::GetInstance()->RewriteURLIfNecessary(
      url, browser_context);
  return true;
}

bool ReverseCompassURL(GURL* url, content::BrowserContext* browser_context) {
  // Keep the committed URL: it's what upstream expects a tab's URL to be.
  return true;
}

}  // namespace

void AddCompassURLHandler(content::BrowserURLHandler* handler) {
  handler->AddHandlerPair(&RewriteCompassURL, &ReverseCompassURL);
}

}  // namespace compass
