// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/strata_sidebar_view.h"

#include <memory>
#include <optional>

#include "base/check.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/webui/webui_embedding_context.h"
#include "strata/browser/ui/webui/sidebar/strata_sidebar_ui.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/view.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/view_utils.h"
#include "url/gurl.h"

namespace strata {

StrataSidebarView::StrataSidebarView(BrowserWindowInterface* browser)
    : views::WebView(browser->GetProfile()) {
  // The page asks for its window as soon as it connects, so set it first.
  webui::SetBrowserWindowInterface(GetWebContents(), browser);
  LoadInitialURL(GURL(kStrataSidebarURL));

  SetProperty(views::kFlexBehaviorKey,
              views::FlexSpecification(views::LayoutOrientation::kVertical,
                                       views::MinimumFlexSizeRule::kScaleToZero,
                                       views::MaximumFlexSizeRule::kUnbounded)
                  .WithWeight(1));
}

StrataSidebarView::~StrataSidebarView() = default;

BEGIN_METADATA(StrataSidebarView)
END_METADATA

void InstallSidebar(views::View* content_parent,
                    views::View* native_tab_strip,
                    views::View* bottom_container,
                    BrowserWindowInterface* browser) {
  CHECK(content_parent);
  CHECK(native_tab_strip);
  CHECK(bottom_container);
  native_tab_strip->SetVisible(false);

  // Upstream lets the bottom container take all the space under the strip.
  // FlexLayout only gives zero-preferred-size views (the sidebar's WebView)
  // what is left over, so cap it at its preferred height.
  bottom_container->SetProperty(
      views::kFlexBehaviorKey,
      views::FlexSpecification(views::MinimumFlexSizeRule::kPreferred,
                               views::MaximumFlexSizeRule::kPreferred));

  std::optional<size_t> strip_index =
      content_parent->GetIndexOf(native_tab_strip);
  CHECK(strip_index.has_value());

  for (views::View* child : content_parent->children()) {
    if (views::IsViewClass<StrataSidebarView>(child)) {
      content_parent->ReorderChildView(child, strip_index.value() + 1);
      return;
    }
  }
  content_parent->AddChildViewAt(std::make_unique<StrataSidebarView>(browser),
                                 strip_index.value() + 1);
}

}  // namespace strata
