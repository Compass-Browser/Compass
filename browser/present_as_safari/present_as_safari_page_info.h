// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_PRESENT_AS_SAFARI_PRESENT_AS_SAFARI_PAGE_INFO_H_
#define COMPASS_BROWSER_PRESENT_AS_SAFARI_PRESENT_AS_SAFARI_PAGE_INFO_H_

namespace content {
class WebContents;
}

namespace views {
class View;
}

namespace compass {

// Adds the "Present as Safari" switch to the page info popup's site settings
// for `contents`' site, on macOS with FairPlay
// (patches/chrome-browser-ui-views-page_info-page_info_main_view.cc.patch).
void AddPresentAsSafariPageInfoRow(views::View* container,
                                   content::WebContents* contents);

}  // namespace compass

#endif  // COMPASS_BROWSER_PRESENT_AS_SAFARI_PRESENT_AS_SAFARI_PAGE_INFO_H_
