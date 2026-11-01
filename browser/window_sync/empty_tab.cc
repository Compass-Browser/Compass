// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/window_sync/empty_tab.h"

#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/common/webui_url_constants.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace compass {

bool IsEmptyTabURL(const GURL& url) {
  if (!url.SchemeIs(content::kChromeUIScheme)) {
    return false;
  }
  return url.host() == kEmptyHost ||
         url.host() == chrome::kChromeUINewTabHost ||
         url.host() == chrome::kChromeUINewTabPageHost;
}

bool IsEmptyTab(content::WebContents* contents) {
  // The visible URL: a new tab's pending one, then chrome://newtab/ (the
  // virtual URL), or the page a navigation away is about to show.
  return contents && IsEmptyTabURL(contents->GetVisibleURL());
}

bool IsEmptyTab(const tabs::TabInterface* tab) {
  return tab && IsEmptyTab(tab->GetContents());
}

void OpenEmptyTab(BrowserWindowInterface* browser) {
  NavigateParams params(browser, GURL(kEmptyURL),
                        ui::PAGE_TRANSITION_AUTO_TOPLEVEL);
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&params);
}

}  // namespace compass
