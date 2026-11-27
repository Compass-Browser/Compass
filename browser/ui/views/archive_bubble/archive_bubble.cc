// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/archive_bubble/archive_bubble.h"

#include <memory>

#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/time/time.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/bubble/webui_bubble_dialog_view.h"
#include "chrome/browser/ui/webui/top_chrome/webui_contents_wrapper.h"
#include "chrome/grit/generated_resources.h"
#include "compass/browser/ui/views/compass_bubble.h"
#include "compass/browser/ui/webui/archive/archive_ui.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

namespace compass {

namespace {

base::WeakPtr<WebUIBubbleDialogView>& OpenBubble() {
  static base::NoDestructor<base::WeakPtr<WebUIBubbleDialogView>> bubble;
  return *bubble;
}

// When the bubble last closed. A press on the archive button while it's
// open closes it first (it loses activation); the click that follows
// mustn't open it again.
class CloseTracker : public views::WidgetObserver {
 public:
  static CloseTracker& Get() {
    static base::NoDestructor<CloseTracker> tracker;
    return *tracker;
  }

  void Track(views::Widget* widget) {
    observation_.Reset();
    if (widget) {
      observation_.Observe(widget);
    }
  }

  bool JustClosed() const {
    return base::TimeTicks::Now() - closed_at_ < base::Milliseconds(500);
  }

  // views::WidgetObserver:
  void OnWidgetDestroying(views::Widget* widget) override {
    closed_at_ = base::TimeTicks::Now();
    observation_.Reset();
  }

 private:
  base::TimeTicks closed_at_;
  base::ScopedObservation<views::Widget, views::WidgetObserver> observation_{
      this};
};

}  // namespace

void ToggleArchiveBubble(BrowserWindowInterface* browser,
                         const gfx::Rect& anchor) {
  if (OpenBubble()) {
    CloseCompassBubble(OpenBubble());
    return;
  }
  if (CloseTracker::Get().JustClosed()) {
    return;
  }
  auto contents = std::make_unique<WebUIContentsWrapperT<CompassArchiveUI>>(
      ArchiveUrl(), browser->GetProfile(), IDS_TASK_MANAGER_TOOL_PREFIX,
      /*esc_closes_ui=*/true);
  OpenBubble() = ShowCompassBubble(browser, std::move(contents), anchor);
  if (OpenBubble()) {
    CloseTracker::Get().Track(OpenBubble()->GetWidget());
  }
}

content::WebContents* GetArchiveContentsForTesting() {
  return GetCompassBubbleContents(OpenBubble());
}

}  // namespace compass
