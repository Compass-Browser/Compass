// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/compass_bubble.h"

#include <optional>
#include <utility>

#include "chrome/browser/platform_util.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/views/bubble/webui_bubble_dialog_view.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/browser/ui/webui/top_chrome/webui_contents_wrapper.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "compass/browser/ui/views/bubble_look.h"
#include "ui/base/base_window.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/color/color_variant.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/bubble/bubble_dialog_delegate_view.h"
#include "ui/views/controls/webview/webview.h"
#include "ui/views/widget/widget.h"

namespace compass {

namespace {

// A WebUI bubble that owns its page: each page is for one item, so it goes
// with the bubble.
class CompassBubbleView : public WebUIBubbleDialogView {
  METADATA_HEADER(CompassBubbleView, WebUIBubbleDialogView)

 public:
  CompassBubbleView(std::unique_ptr<WebUIContentsWrapper> contents,
                    const gfx::Rect& anchor,
                    views::BubbleBorder::Arrow arrow,
                    std::optional<SkColor> tint)
      : WebUIBubbleDialogView(nullptr, contents->GetWeakPtr(), anchor, arrow),
        contents_(std::move(contents)),
        tint_(tint) {}
  CompassBubbleView(const CompassBubbleView&) = delete;
  CompassBubbleView& operator=(const CompassBubbleView&) = delete;

  ~CompassBubbleView() override {
    // Before the page goes: the web view still shows it.
    ClearContentsWrapper();
  }

  // views::View:
  void OnThemeChanged() override {
    WebUIBubbleDialogView::OnThemeChanged();
    // The frame around the page: the page's own background (the pages'
    // GetLook()).
    SetBackgroundColor(ui::ColorVariant(
        ComputeBubbleLook(GetColorProvider(), tint_).background));
  }

 private:
  const std::unique_ptr<WebUIContentsWrapper> contents_;
  const std::optional<SkColor> tint_;
};

BEGIN_METADATA(CompassBubbleView)
END_METADATA

}  // namespace

base::WeakPtr<WebUIBubbleDialogView> ShowCompassBubble(
    BrowserWindowInterface* browser,
    std::unique_ptr<WebUIContentsWrapper> contents,
    const gfx::Rect& anchor) {
  // The page takes the window's space's tint (its GetLook()).
  webui::SetBrowserWindowInterface(contents->web_contents(), browser);

  // Up from the switcher at the bottom of the sidebar, down from its rows.
  views::BubbleBorder::Arrow arrow = views::BubbleBorder::LEFT_TOP;
  if (BrowserView* view = BrowserView::GetBrowserViewForBrowser(browser)) {
    if (anchor.CenterPoint().y() > view->GetBoundsInScreen().CenterPoint().y()) {
      arrow = views::BubbleBorder::LEFT_BOTTOM;
    }
  }

  auto bubble = std::make_unique<CompassBubbleView>(
      std::move(contents), anchor, arrow, GetBubbleTint(browser));
  bubble->set_parent_window(platform_util::GetViewForWindow(
      browser->GetWindow()->GetNativeWindow()));
  base::WeakPtr<WebUIBubbleDialogView> weak = bubble->GetWeakPtr();
  // Hidden until the page has drawn (its handler's ShowUI()).
  views::BubbleDialogDelegateView::CreateBubble(std::move(bubble));
  return weak;
}

void CloseCompassBubble(base::WeakPtr<WebUIBubbleDialogView> bubble) {
  if (bubble && bubble->GetWidget()) {
    bubble->GetWidget()->CloseNow();
  }
}

content::WebContents* GetCompassBubbleContents(
    base::WeakPtr<WebUIBubbleDialogView> bubble) {
  return bubble && bubble->web_view() ? bubble->web_view()->web_contents()
                                      : nullptr;
}

}  // namespace compass
