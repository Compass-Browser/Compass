// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_WINDOW_SYNC_EMPTY_TAB_H_
#define COMPASS_BROWSER_WINDOW_SYNC_EMPTY_TAB_H_

class BrowserWindowInterface;
class GURL;

namespace content {
class WebContents;
}  // namespace content

namespace tabs {
class TabInterface;
}  // namespace tabs

namespace compass {

// chrome://compass-empty: what a window shows when it has nothing else to
// show, under its command bar. There's no new tab page: chrome://newtab loads
// this page too (compass_url_handler.h). An empty tab has no entry in the
// sidebar, and it closes once the window shows another tab. See
// docs/design/command-bar.md, "No new tab page".
inline constexpr char kEmptyHost[] = "compass-empty";
inline constexpr char kEmptyURL[] = "chrome://compass-empty/";

// chrome://compass-empty/, or a new tab page URL (chrome://newtab/ is what an
// empty tab shows as).
bool IsEmptyTabURL(const GURL& url);

// Whether |contents| shows, or is about to show, the empty page.
bool IsEmptyTab(content::WebContents* contents);
bool IsEmptyTab(const tabs::TabInterface* tab);

// Opens an empty tab in |browser|, in the foreground.
void OpenEmptyTab(BrowserWindowInterface* browser);

}  // namespace compass

#endif  // COMPASS_BROWSER_WINDOW_SYNC_EMPTY_TAB_H_
