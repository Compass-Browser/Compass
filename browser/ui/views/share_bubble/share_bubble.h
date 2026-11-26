// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_SHARE_BUBBLE_SHARE_BUBBLE_H_
#define COMPASS_BROWSER_UI_VIEWS_SHARE_BUBBLE_SHARE_BUBBLE_H_

#include <string>

class BrowserWindowInterface;

namespace content {
class WebContents;
}  // namespace content

namespace gfx {
class Rect;
}  // namespace gfx

namespace compass {

// Shows the share sheet of a space, folder or easel (chrome://compass-share,
// docs/design/sync.md, "Rooms: clients") in a bubble of |browser|'s window,
// pointing at |anchor| (in screen coordinates): the sidebar's row it was
// opened from (ShowCompassBubble()). It looks like the downloads bubble: the
// window's space's tint, captioned cards. A share bubble that's open closes
// first.
void ShowShareBubble(BrowserWindowInterface* browser,
                     const std::string& kind,
                     const std::string& id,
                     const gfx::Rect& anchor);

// The open share bubble's page, or null.
content::WebContents* GetShareBubbleContentsForTesting();

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_SHARE_BUBBLE_SHARE_BUBBLE_H_
