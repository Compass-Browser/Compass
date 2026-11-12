// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Upstream features Compass removes: bookmarks and the reading list (pinned
// tabs are the bookmarks), horizontal tabs, tab search and parts of the app
// menu (compass::IsRemovedCommand()).

#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/recent_tabs_sub_menu_model.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/toolbar/app_menu_model.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/bookmarks/common/bookmark_pref_names.h"
#include "components/prefs/pref_service.h"
#include "components/strings/grit/components_strings.h"
#include "content/public/browser/webui_config_map.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/l10n/l10n_util.h"
#include "url/gurl.h"

namespace compass {
namespace {

using CompassRemovedFeaturesBrowserTest = InProcessBrowserTest;

IN_PROC_BROWSER_TEST_F(CompassRemovedFeaturesBrowserTest, NoBookmarks) {
  for (int command :
       {IDC_BOOKMARK_THIS_TAB, IDC_BOOKMARK_ALL_TABS, IDC_SHOW_BOOKMARK_BAR,
        IDC_SHOW_BOOKMARK_MANAGER, IDC_SHOW_BOOKMARK_SIDE_PANEL,
        IDC_BOOKMARKS_MENU}) {
    EXPECT_FALSE(chrome::SupportsCommand(browser(), command)) << command;
    EXPECT_FALSE(chrome::IsCommandEnabled(browser(), command)) << command;
    EXPECT_FALSE(chrome::ExecuteCommand(browser(), command)) << command;
  }
  EXPECT_FALSE(chrome::CanBookmarkCurrentTab(browser()));

  // The bar stays hidden even with Chrome's "always show" pref.
  browser()->GetProfile()->GetPrefs()->SetBoolean(
      bookmarks::prefs::kShowBookmarkBar, true);
  EXPECT_FALSE(
      BrowserView::GetBrowserViewForBrowser(browser())->IsBookmarkBarVisible());

  EXPECT_FALSE(content::WebUIConfigMap::GetInstance().GetConfig(
      browser()->GetProfile(), GURL(chrome::kChromeUIBookmarksURL)));
}

IN_PROC_BROWSER_TEST_F(CompassRemovedFeaturesBrowserTest, NoReadingList) {
  for (int command : {IDC_READING_LIST_MENU, IDC_READING_LIST_MENU_ADD_TAB,
                      IDC_READING_LIST_MENU_SHOW_UI}) {
    EXPECT_FALSE(chrome::SupportsCommand(browser(), command)) << command;
    EXPECT_FALSE(chrome::IsCommandEnabled(browser(), command)) << command;
    EXPECT_FALSE(chrome::ExecuteCommand(browser(), command)) << command;
  }
}

IN_PROC_BROWSER_TEST_F(CompassRemovedFeaturesBrowserTest,
                       TabsStayVertical) {
  tabs::VerticalTabStripStateController* controller =
      tabs::VerticalTabStripStateController::From(browser());
  ASSERT_TRUE(controller);
  EXPECT_TRUE(controller->ShouldDisplayVerticalTabs());

  EXPECT_FALSE(chrome::SupportsCommand(browser(), IDC_TOGGLE_VERTICAL_TABS));
  EXPECT_FALSE(chrome::ExecuteCommand(browser(), IDC_TOGGLE_VERTICAL_TABS));

  controller->SetVerticalTabsEnabled(false);
  EXPECT_TRUE(controller->ShouldDisplayVerticalTabs());

  // A direct pref write is undone.
  PrefService* prefs = browser()->GetProfile()->GetPrefs();
  prefs->SetBoolean(prefs::kVerticalTabsEnabled, false);
  EXPECT_TRUE(prefs->GetBoolean(prefs::kVerticalTabsEnabled));
  EXPECT_TRUE(controller->ShouldDisplayVerticalTabs());
}

// The app menu has no profile row, no tab groups menu and no "Search tabs"
// under More tools; History has no "Your devices".
IN_PROC_BROWSER_TEST_F(CompassRemovedFeaturesBrowserTest, AppMenuLeavesOut) {
  EXPECT_FALSE(chrome::SupportsCommand(browser(), IDC_TAB_SEARCH));
  EXPECT_FALSE(chrome::ExecuteCommand(browser(), IDC_TAB_SEARCH));

  AppMenuModel model(/*provider=*/nullptr, browser());
  model.Init();
  EXPECT_FALSE(
      model.GetIndexOfCommandId(AppMenuModel::kProfileMenuPlaceholder));
  EXPECT_FALSE(
      model.GetIndexOfCommandId(AppMenuModel::kSavedTabGroupsMenuPlaceholder));
  EXPECT_TRUE(model.GetIndexOfCommandId(AppMenuModel::kRecentTabsMenuPlaceholder));

  ToolsMenuModel tools(&model, browser());
  EXPECT_TRUE(tools.GetItemCount());
  EXPECT_FALSE(tools.GetIndexOfCommandId(IDC_TAB_SEARCH));

  RecentTabsSubMenuModel history(/*accelerator_provider=*/nullptr, browser());
  const std::u16string devices = l10n_util::GetStringUTF16(IDS_YOUR_DEVICES);
  for (size_t i = 0; i < history.GetItemCount(); ++i) {
    EXPECT_NE(history.GetLabelAt(i), devices) << i;
  }
}

}  // namespace
}  // namespace compass
