// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/share_bubble/share_bubble.h"

#include <memory>

#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/bubble/webui_bubble_dialog_view.h"
#include "chrome/browser/ui/webui/top_chrome/webui_contents_wrapper.h"
#include "chrome/grit/generated_resources.h"
#include "compass/browser/ui/views/compass_bubble.h"
#include "compass/browser/ui/webui/share/compass_share_ui.h"
#include "ui/gfx/geometry/rect.h"

namespace compass {

namespace {

base::WeakPtr<WebUIBubbleDialogView>& OpenBubble() {
  static base::NoDestructor<base::WeakPtr<WebUIBubbleDialogView>> bubble;
  return *bubble;
}

}  // namespace

void ShowShareBubble(BrowserWindowInterface* browser,
                     const std::string& kind,
                     const std::string& id,
                     const gfx::Rect& anchor) {
  CloseCompassBubble(OpenBubble());
  OpenBubble() = ShowCompassBubble(
      browser,
      std::make_unique<WebUIContentsWrapperT<CompassShareUI>>(
          ShareSheetUrl(kind, id), browser->GetProfile(),
          IDS_TASK_MANAGER_TOOL_PREFIX),
      anchor);
}

content::WebContents* GetShareBubbleContentsForTesting() {
  return GetCompassBubbleContents(OpenBubble());
}

}  // namespace compass
