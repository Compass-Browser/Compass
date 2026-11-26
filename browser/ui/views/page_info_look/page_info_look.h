// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_PAGE_INFO_LOOK_PAGE_INFO_LOOK_H_
#define COMPASS_BROWSER_UI_VIEWS_PAGE_INFO_LOOK_PAGE_INFO_LOOK_H_

namespace content {
class WebContents;
}  // namespace content

namespace views {
class BubbleDialogDelegateView;
class View;
}  // namespace views

namespace compass {

// Gives upstream's site information popup the downloads bubble's look
// (docs/design/downloads-bubble.md, "Look"): the window's space's tint on
// the bubble, and each page's sections (everything under its header) as
// rounded cards of translucent white, their separators gone. |pages| is the
// bubble's PageSwitcherView: every page it switches to is restyled, and so
// are sections as rows come and go. Upstream's PageInfoBubbleView calls it
// once its widget exists (see
// patches/chrome-browser-ui-views-page_info-page_info_bubble_view.cc.patch);
// declared in a target with no dependencies, defined in //compass/browser/ui.
void InstallPageInfoLook(views::BubbleDialogDelegateView* bubble,
                         views::View* pages,
                         content::WebContents* contents);

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_PAGE_INFO_LOOK_PAGE_INFO_LOOK_H_
