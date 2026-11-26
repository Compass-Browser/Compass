// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_SPACE_EDITOR_BUBBLE_SPACE_EDITOR_BUBBLE_H_
#define COMPASS_BROWSER_UI_VIEWS_SPACE_EDITOR_BUBBLE_SPACE_EDITOR_BUBBLE_H_

#include <string>

class BrowserWindowInterface;

namespace content {
class WebContents;
}  // namespace content

namespace gfx {
class Rect;
}  // namespace gfx

namespace compass {

// Shows the editor of space |space_id| (chrome://compass-space-editor: its
// name, theme, icon, archiving, profile, sync, sharing and deletion;
// docs/design/spaces.md, "UI") in a bubble of |browser|'s window, pointing
// at |anchor| (in screen coordinates): the switcher's button or the space's
// name it was opened from (ShowCompassBubble()). One that's open closes
// first.
void ShowSpaceEditorBubble(BrowserWindowInterface* browser,
                           const std::string& space_id,
                           const gfx::Rect& anchor);

// The open space editor's page, or null.
content::WebContents* GetSpaceEditorContentsForTesting();

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_SPACE_EDITOR_BUBBLE_SPACE_EDITOR_BUBBLE_H_
