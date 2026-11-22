// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_ADBLOCK_ADBLOCK_PAGE_INFO_H_
#define COMPASS_BROWSER_UI_VIEWS_ADBLOCK_ADBLOCK_PAGE_INFO_H_

namespace content {
class WebContents;
}  // namespace content

namespace views {
class View;
}  // namespace views

namespace compass {

// Adds the ad blocker's switch for |contents|' site to the site information
// popup's |container| (docs/design/ad-blocker.md), for a web page while the
// ad blocker is on. Upstream's PageInfoMainView calls it (see
// patches/chrome-browser-ui-views-page_info-page_info_main_view.cc.patch);
// declared in a target with no dependencies, defined in //compass/browser/ui.
void AddAdBlockPageInfoRow(views::View* container,
                           content::WebContents* contents);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_ADBLOCK_ADBLOCK_PAGE_INFO_H_
