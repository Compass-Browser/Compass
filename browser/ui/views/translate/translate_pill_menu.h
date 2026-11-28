// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_TRANSLATE_TRANSLATE_PILL_MENU_H_
#define COMPASS_BROWSER_UI_VIEWS_TRANSLATE_TRANSLATE_PILL_MENU_H_

#include <memory>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "ui/menus/simple_menu_model.h"

namespace content {
class WebContents;
}

namespace views {
class MenuRunner;
class View;
}  // namespace views

namespace compass {

// The pill's translate button's menu (docs/design/translation.md, "What
// the user sees"), for a translated page or one that failed: "Translated
// from German", Translate To ▸ (the languages Compass has), Show Original
// or Try Again, Always Translate German, Never Translate German, Never
// Translate This Site.
class TranslatePillMenu : public ui::SimpleMenuModel::Delegate {
 public:
  explicit TranslatePillMenu(content::WebContents* web_contents);
  TranslatePillMenu(const TranslatePillMenu&) = delete;
  TranslatePillMenu& operator=(const TranslatePillMenu&) = delete;
  ~TranslatePillMenu() override;

  void Show(views::View* anchor);

  // ui::SimpleMenuModel::Delegate:
  bool IsCommandIdChecked(int command_id) const override;
  bool IsCommandIdEnabled(int command_id) const override;
  void ExecuteCommand(int command_id, int event_flags) override;

 private:
  void Build();

  base::WeakPtr<content::WebContents> web_contents_;
  std::string source_;
  std::string target_;
  std::vector<std::string> targets_;
  std::unique_ptr<ui::SimpleMenuModel> model_;
  std::unique_ptr<ui::SimpleMenuModel> targets_model_;
  std::unique_ptr<views::MenuRunner> runner_;
};

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_TRANSLATE_TRANSLATE_PILL_MENU_H_
