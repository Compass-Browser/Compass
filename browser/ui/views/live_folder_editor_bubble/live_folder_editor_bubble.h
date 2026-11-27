// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_LIVE_FOLDER_EDITOR_BUBBLE_LIVE_FOLDER_EDITOR_BUBBLE_H_
#define COMPASS_BROWSER_UI_VIEWS_LIVE_FOLDER_EDITOR_BUBBLE_LIVE_FOLDER_EDITOR_BUBBLE_H_

#include <optional>
#include <string>

class BrowserWindowInterface;

namespace content {
class WebContents;
}  // namespace content

namespace gfx {
class Rect;
}  // namespace gfx

namespace compass {

// Shows the live folder editor (chrome://compass-live-folder-editor;
// docs/design/live-folders.md, "UI") in a bubble of |browser|'s window,
// pointing at |anchor| (in screen coordinates): the row or button it was
// opened from (ShowCompassBubble()). For a new folder in |parent_id|, or
// for folder |folder_id|. One that's open closes first.
void ShowLiveFolderEditorBubble(BrowserWindowInterface* browser,
                                const std::optional<std::string>& parent_id,
                                const std::optional<std::string>& folder_id,
                                const gfx::Rect& anchor);

// The open editor's page, or null.
content::WebContents* GetLiveFolderEditorContentsForTesting();

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_LIVE_FOLDER_EDITOR_BUBBLE_LIVE_FOLDER_EDITOR_BUBBLE_H_
