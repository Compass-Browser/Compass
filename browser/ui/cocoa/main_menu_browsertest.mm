// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The macOS menu bar leaves out what Compass doesn't have: the toolbar's
// toggles, Home, the Profiles menu and tab groups.

#import <AppKit/AppKit.h>

#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

using CompassMainMenuBrowserTest = InProcessBrowserTest;

NSMenu* Submenu(int tag) {
  return [NSApp.mainMenu itemWithTag:tag].submenu;
}

IN_PROC_BROWSER_TEST_F(CompassMainMenuBrowserTest, LeavesOutUnusedItems) {
  NSMenu* view = Submenu(IDC_VIEW_MENU);
  ASSERT_TRUE(view);
  EXPECT_TRUE([view itemWithTag:IDC_RELOAD]);
  EXPECT_TRUE([view itemWithTag:IDC_TOGGLE_VERTICAL_TABS_COLLAPSE]);
  for (int tag : {IDC_TOGGLE_FULLSCREEN_TOOLBAR, IDC_SHOW_FULL_URLS,
                  IDC_SHOW_GOOGLE_LENS_SHORTCUT,
                  IDC_SHOW_AI_MODE_OMNIBOX_BUTTON, IDC_SHOW_SEARCH_TOOLS}) {
    EXPECT_FALSE([view itemWithTag:tag]) << tag;
  }

  NSMenu* history = Submenu(IDC_HISTORY_MENU);
  ASSERT_TRUE(history);
  EXPECT_TRUE([history itemWithTag:IDC_BACK]);
  EXPECT_FALSE([history itemWithTag:IDC_HOME]);

  NSMenu* tab = Submenu(IDC_TAB_MENU);
  ASSERT_TRUE(tab);
  EXPECT_TRUE([tab itemWithTag:IDC_DUPLICATE_TAB]);
  EXPECT_FALSE([tab itemWithTag:IDC_WINDOW_GROUP_TAB]);
  EXPECT_FALSE([tab itemWithTag:IDC_GROUP_TARGET_TAB]);
  EXPECT_FALSE(chrome::SupportsCommand(browser(), IDC_WINDOW_GROUP_TAB));
  EXPECT_FALSE(chrome::SupportsCommand(browser(), IDC_GROUP_TARGET_TAB));

  // Hidden rather than removed: the profile menu controller keeps it.
  NSMenuItem* profiles = [NSApp.mainMenu itemWithTag:IDC_PROFILE_MAIN_MENU];
  ASSERT_TRUE(profiles);
  EXPECT_TRUE(profiles.hidden);
}

}  // namespace
}  // namespace compass
