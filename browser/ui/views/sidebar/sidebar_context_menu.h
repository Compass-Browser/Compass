// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_CONTEXT_MENU_H_
#define COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_CONTEXT_MENU_H_

#include <memory>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "content/public/browser/web_contents_user_data.h"
#include "ui/gfx/geometry/point.h"

namespace ui {
class MenuModel;
}  // namespace ui

namespace views {
class MenuRunner;
class View;
}  // namespace views

namespace compass {

// Runs the context menus of the sidebar's entries: native menus, at the point
// in the sidebar the page was right-clicked. The page says where, and the page
// handler builds the menu (SidebarPageHandler::ShowContextMenu()). Lives on
// the sidebar page's WebContents, which CompassSidebarView owns. See
// docs/design/sidebar.md, "Context menus".
class SidebarContextMenu
    : public content::WebContentsUserData<SidebarContextMenu> {
 public:
  // Gets the menus instead of running them: the model and the point in the
  // sidebar.
  using ShowCallback =
      base::RepeatingCallback<void(ui::MenuModel*, const gfx::Point&)>;

  SidebarContextMenu(const SidebarContextMenu&) = delete;
  SidebarContextMenu& operator=(const SidebarContextMenu&) = delete;
  ~SidebarContextMenu() override;

  // Shows |model| at |point| in the sidebar, closing a menu that's up.
  void Show(std::unique_ptr<ui::MenuModel> model, const gfx::Point& point);

  static void SetShowCallbackForTesting(ShowCallback callback);

 private:
  friend WebContentsUserData;

  // |sidebar| hosts |contents|, and outlives it.
  SidebarContextMenu(content::WebContents* contents, views::View* sidebar);

  const raw_ptr<views::View> sidebar_;
  // The runner goes first: it may still use the model.
  std::unique_ptr<ui::MenuModel> model_;
  std::unique_ptr<views::MenuRunner> runner_;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_SIDEBAR_SIDEBAR_CONTEXT_MENU_H_
