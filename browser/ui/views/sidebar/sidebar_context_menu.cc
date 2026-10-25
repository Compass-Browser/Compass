// Copyright 2026 The Strata Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "strata/browser/ui/views/sidebar/sidebar_context_menu.h"

#include <utility>

#include "base/no_destructor.h"
#include "ui/base/models/menu_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace strata {

namespace {

SidebarContextMenu::ShowCallback& GetShowCallbackForTesting() {
  static base::NoDestructor<SidebarContextMenu::ShowCallback> callback;
  return *callback;
}

}  // namespace

SidebarContextMenu::SidebarContextMenu(content::WebContents* contents,
                                       views::View* sidebar)
    : content::WebContentsUserData<SidebarContextMenu>(*contents),
      sidebar_(sidebar) {}

SidebarContextMenu::~SidebarContextMenu() {
  runner_.reset();
}

void SidebarContextMenu::Show(std::unique_ptr<ui::MenuModel> model,
                              const gfx::Point& point) {
  runner_.reset();
  model_ = std::move(model);
  if (const ShowCallback& callback = GetShowCallbackForTesting()) {
    callback.Run(model_.get(), point);
    return;
  }
  views::Widget* widget = sidebar_->GetWidget();
  if (!widget) {
    return;
  }
  gfx::Point screen_point = point;
  views::View::ConvertPointToScreen(sidebar_, &screen_point);
  runner_ = std::make_unique<views::MenuRunner>(
      model_.get(),
      views::MenuRunner::CONTEXT_MENU | views::MenuRunner::HAS_MNEMONICS);
  runner_->RunMenuAt(widget, /*button_controller=*/nullptr,
                     gfx::Rect(screen_point, gfx::Size()),
                     views::MenuAnchorPosition::kTopLeft,
                     ui::mojom::MenuSourceType::kMouse);
}

// static
void SidebarContextMenu::SetShowCallbackForTesting(ShowCallback callback) {
  GetShowCallbackForTesting() = std::move(callback);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(SidebarContextMenu);

}  // namespace strata
