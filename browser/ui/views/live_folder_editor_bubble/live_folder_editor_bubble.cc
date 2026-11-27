// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/live_folder_editor_bubble/live_folder_editor_bubble.h"

#include <memory>

#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/bubble/webui_bubble_dialog_view.h"
#include "chrome/browser/ui/webui/top_chrome/webui_contents_wrapper.h"
#include "chrome/grit/generated_resources.h"
#include "compass/browser/ui/views/compass_bubble.h"
#include "compass/browser/ui/webui/live_folder_editor/live_folder_editor_ui.h"
#include "ui/gfx/geometry/rect.h"

namespace compass {

namespace {

base::WeakPtr<WebUIBubbleDialogView>& OpenBubble() {
  static base::NoDestructor<base::WeakPtr<WebUIBubbleDialogView>> bubble;
  return *bubble;
}

}  // namespace

void ShowLiveFolderEditorBubble(BrowserWindowInterface* browser,
                                const std::optional<std::string>& parent_id,
                                const std::optional<std::string>& folder_id,
                                const gfx::Rect& anchor) {
  CloseCompassBubble(OpenBubble());
  auto contents =
      std::make_unique<WebUIContentsWrapperT<CompassLiveFolderEditorUI>>(
          LiveFolderEditorUrl(parent_id, folder_id), browser->GetProfile(),
          IDS_TASK_MANAGER_TOOL_PREFIX,
          // The page's Esc backs out of a plugin's form and the install
          // review first.
          /*esc_closes_ui=*/false);
  OpenBubble() = ShowCompassBubble(browser, std::move(contents), anchor);
}

content::WebContents* GetLiveFolderEditorContentsForTesting() {
  return GetCompassBubbleContents(OpenBubble());
}

}  // namespace compass
