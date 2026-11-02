// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Upstream features Compass removes: bookmarks (pinned tabs are the
// bookmarks) and horizontal tabs (compass::IsRemovedCommand()).

#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_command_controller.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/tabs/vertical_tab_strip_state_controller.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "components/bookmarks/common/bookmark_pref_names.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/webui_config_map.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
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

}  // namespace
}  // namespace compass
