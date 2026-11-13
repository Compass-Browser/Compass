// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The macOS menu bar: what it leaves out (the toolbar's toggles, Home, the
// Profiles menu and tab groups) and the Spaces menu.

#import <AppKit/AppKit.h>

#include "base/strings/sys_string_conversions.h"
#include "base/test/run_until.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/test/browser_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/gtest_mac.h"

#include "compass/browser/sidebar/space_model.h"
#import "compass/browser/ui/cocoa/spaces_menu_mac.h"
#include "compass/browser/window_sync/window_features.h"

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

// The window's spaces with their shortcuts and a check on the shown one,
// then New Space, which adds one and shows it.
IN_PROC_BROWSER_TEST_F(CompassMainMenuBrowserTest, SpacesMenu) {
  WindowFeatures* features = WindowFeatures::From(browser());
  ASSERT_TRUE(features);
  NSMenu* spaces = Submenu(kSpacesMenuTag);
  ASSERT_TRUE(spaces);
  // Between the Tab and Window menus.
  NSMenu* main = NSApp.mainMenu;
  EXPECT_EQ([main indexOfItemWithTag:kSpacesMenuTag],
            [main indexOfItemWithTag:IDC_TAB_MENU] + 1);

  [spaces.delegate menuNeedsUpdate:spaces];
  // One space, a separator and New Space.
  ASSERT_EQ(3, spaces.numberOfItems);
  NSMenuItem* first = [spaces itemAtIndex:0];
  EXPECT_NSEQ(base::SysUTF16ToNSString(features->space_model().name()),
              first.title);
  EXPECT_EQ(NSControlStateValueOn, first.state);
  EXPECT_NSEQ(@"1", first.keyEquivalent);
  EXPECT_EQ(NSEventModifierFlagControl,
            first.keyEquivalentModifierMask &
                NSEventModifierFlagDeviceIndependentFlagsMask);
  EXPECT_TRUE([spaces itemAtIndex:1].separatorItem);
  NSMenuItem* add = [spaces itemAtIndex:2];
  EXPECT_NSEQ(@"New Space", add.title);

  SpaceModel* old_space = &features->space_model();
  [NSApp sendAction:add.action to:add.target from:add];
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() != old_space; }));
  EXPECT_EQ(u"Space 2", features->space_model().name());

  // Now two spaces, the new one checked, with Ctrl+2.
  [spaces.delegate menuNeedsUpdate:spaces];
  ASSERT_EQ(4, spaces.numberOfItems);
  EXPECT_EQ(NSControlStateValueOff, [spaces itemAtIndex:0].state);
  NSMenuItem* second = [spaces itemAtIndex:1];
  EXPECT_NSEQ(@"Space 2", second.title);
  EXPECT_EQ(NSControlStateValueOn, second.state);
  EXPECT_NSEQ(@"2", second.keyEquivalent);

  // Choosing the first goes back.
  [NSApp sendAction:first.action to:first.target from:first];
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return &features->space_model() == old_space; }));
}

}  // namespace
}  // namespace compass
