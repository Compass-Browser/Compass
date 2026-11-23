// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_EASELS_EASEL_CAPTURE_H_
#define COMPASS_BROWSER_UI_EASELS_EASEL_CAPTURE_H_

#include "compass/components/sync/protocol/compass_spaces_specifics.pb.h"

class BrowserWindowInterface;

namespace content {
class WebContents;
}  // namespace content

namespace compass {

class SidebarNode;

// Capture to Easel and New Easel on the desktop (docs/design/easels.md,
// "In the sidebar" and "Captures"): the command bar, the page's context
// menu and the sidebar's Pinned header call these.
namespace easels {

// Whether Capture to Easel can run on |contents|: a web page (http, https
// or file) of a profile with Easels.
bool CanCaptureToEasel(content::WebContents* contents);

// Puts the overlay on |contents|' page: the user drags a region and picks
// a board (the last used first, then the rest, and New Easel); Esc
// cancels. The region is copied, added to the board, and the page says so
// for a moment. Again while it's up: it starts over.
void StartCaptureToEasel(content::WebContents* contents);

// Capture Screenshot (docs/design/screenshots.md): whether it can run on
// |contents' page (http, https or file; off the record too), and the
// overlay to drag a region or take the whole page, then copy or save it.
bool CanCaptureScreenshot(content::WebContents* contents);
void StartCaptureScreenshot(content::WebContents* contents);

// New Easel: a board, with a tab for it pinned at the end of |browser|'s
// space's Pinned (Today's top in a space without Pinned), shown.
void NewEaselInWindow(BrowserWindowInterface* browser);

// The pinned tab entry for |easel| in |browser|'s space, unloaded; null
// when the window has no space for it (the parking window, mid-swap).
SidebarNode* PinEaselTab(BrowserWindowInterface* browser,
                         const sync_pb::CompassEasel& easel);

}  // namespace easels
}  // namespace compass

#endif  // COMPASS_BROWSER_UI_EASELS_EASEL_CAPTURE_H_
