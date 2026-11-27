// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_ARCHIVE_BUBBLE_ARCHIVE_BUBBLE_H_
#define COMPASS_BROWSER_UI_VIEWS_ARCHIVE_BUBBLE_ARCHIVE_BUBBLE_H_

class BrowserWindowInterface;

namespace content {
class WebContents;
}  // namespace content

namespace gfx {
class Rect;
}  // namespace gfx

namespace compass {

// Shows the window's space's archive (chrome://compass-archive;
// docs/design/sidebar.md, "Today auto-archive") in a bubble of |browser|'s
// window, pointing at |anchor| (in screen coordinates): Today's archive
// button (ShowCompassBubble()). If it's open already, closes it instead:
// the button toggles it.
void ToggleArchiveBubble(BrowserWindowInterface* browser,
                         const gfx::Rect& anchor);

// The open archive's page, or null.
content::WebContents* GetArchiveContentsForTesting();

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_ARCHIVE_BUBBLE_ARCHIVE_BUBBLE_H_
