// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"

#include <memory>

#include "base/check.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/controls/webview/web_contents_set_background_color.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_utils.h"
#include "url/gurl.h"

namespace strata {

namespace {

// Takes whatever height the region's content area has left.
views::FlexSpecification SidebarFlex() {
  return views::FlexSpecification(views::LayoutOrientation::kVertical,
                                  views::MinimumFlexSizeRule::kScaleToZero,
                                  views::MaximumFlexSizeRule::kUnbounded)
      .WithWeight(1);
}

}  // namespace

StrataSidebarView::StrataSidebarView(BrowserWindowInterface* browser)
    : views::WebView(browser->GetProfile()) {
  // The page asks for its window as soon as it connects, so set it first.
  webui::SetBrowserWindowInterface(GetWebContents(), browser);
  // Let the region's frame-colored background show through, so the sidebar
  // follows the theme and the window's active state.
  views::WebContentsSetBackgroundColor::CreateForWebContentsWithColor(
      GetWebContents(), SK_ColorTRANSPARENT);
  LoadInitialURL(GURL(kStrataSidebarURL));
}

StrataSidebarView::~StrataSidebarView() = default;

BEGIN_METADATA(StrataSidebarView)
END_METADATA

std::unique_ptr<views::View> CreateSidebar(BrowserWindowInterface* browser) {
  return std::make_unique<StrataSidebarView>(browser);
}

void InstallSidebar(views::View* sidebar, views::View* bottom_container) {
  CHECK(views::IsViewClass<StrataSidebarView>(sidebar));
  CHECK(bottom_container);
  // The region gives its tab strip the native strip's flex rule and margins.
  sidebar->SetProperty(views::kFlexBehaviorKey, SidebarFlex());
  sidebar->ClearProperty(views::kMarginsKey);

  // Upstream lets the bottom container take all the space under the strip.
  // FlexLayout only gives zero-preferred-size views (the sidebar's WebView)
  // what is left over, so cap it at its preferred height.
  bottom_container->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::MinimumFlexSizeRule::kPreferred,
                               views::MaximumFlexSizeRule::kPreferred));
}

}  // namespace strata
