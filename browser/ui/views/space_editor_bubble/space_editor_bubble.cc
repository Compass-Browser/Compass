// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/space_editor_bubble/space_editor_bubble.h"

#include <memory>

#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/bubble/webui_bubble_dialog_view.h"
#include "chrome/browser/ui/webui/top_chrome/webui_contents_wrapper.h"
#include "chrome/grit/generated_resources.h"
#include "compass/browser/ui/views/compass_bubble.h"
#include "compass/browser/ui/webui/space_editor/space_editor_ui.h"
#include "ui/gfx/geometry/rect.h"

namespace compass {

namespace {

base::WeakPtr<WebUIBubbleDialogView>& OpenBubble() {
  static base::NoDestructor<base::WeakPtr<WebUIBubbleDialogView>> bubble;
  return *bubble;
}

}  // namespace

void ShowSpaceEditorBubble(BrowserWindowInterface* browser,
                           const std::string& space_id,
                           const gfx::Rect& anchor) {
  CloseCompassBubble(OpenBubble());
  auto contents =
      std::make_unique<WebUIContentsWrapperT<CompassSpaceEditorUI>>(
          SpaceEditorUrl(space_id), browser->GetProfile(),
          IDS_TASK_MANAGER_TOOL_PREFIX,
          // The page's Esc goes back from the theme editor and the emoji
          // picker first.
          /*esc_closes_ui=*/false);
  // Its Share opens the share bubble pointing at the same place.
  contents->GetWebUIController()->set_anchor(anchor);
  OpenBubble() = ShowCompassBubble(browser, std::move(contents), anchor);
}

content::WebContents* GetSpaceEditorContentsForTesting() {
  return GetCompassBubbleContents(OpenBubble());
}

}  // namespace compass
