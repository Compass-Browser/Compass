// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// macOS's own little window tests; every platform's are in
// little_window_browsertest.cc.

#include "compass/browser/ui/views/little_window/little_window.h"
#include "compass/browser/ui/views/little_window/little_window_hot_key.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/events/event_constants.h"
#include "ui/events/keycodes/keyboard_codes.h"

#import <Cocoa/Cocoa.h>

#include "base/strings/sys_string_conversions.h"
#include "base/test/run_until.h"
#import "chrome/browser/app_controller_mac.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "compass/browser/ui/views/little_window/little_window_browsertest.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

// A link another app opens (application:openURLs:) shows in a little window,
// not as a tab of the window.
IN_PROC_BROWSER_TEST_F(LittleWindowBrowserTest, LinkFromAnotherApp) {
  const GURL url = embedded_test_server()->GetURL("/title1.html");
  [AppController.sharedController
      application:NSApp
         openURLs:@[ [NSURL URLWithString:base::SysUTF8ToNSString(url.spec())] ]];

  ASSERT_TRUE(base::test::RunUntil([&] { return FindLittleWindow(); }));
  BrowserWindowInterface* little = FindLittleWindow();
  EXPECT_EQ(little->GetType(), BrowserWindowInterface::TYPE_POPUP);
  ASSERT_EQ(little->GetTabStripModel()->count(), 1);
  ASSERT_TRUE(content::WaitForLoadStop(PageOf(little)));
  EXPECT_EQ(PageOf(little)->GetLastCommittedURL(), url);
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
}

// The shortcut is a system hot key too, so it works from any app, and not
// while the settings page records keys. A press opens a little window on
// the new tab page.
IN_PROC_BROWSER_TEST_F(LittleWindowBrowserTest, HotKeyFromAnyApp) {
  LittleWindowHotKey& hot_key = LittleWindowHotKey::Get();
  const ui::Accelerator keys(ui::VKEY_N,
                             ui::EF_COMMAND_DOWN | ui::EF_ALT_DOWN);
  EXPECT_TRUE(hot_key.listening());
  EXPECT_EQ(hot_key.listening_accelerator(), keys);
  ShortcutSettings::Get().SetRecording(true);
  EXPECT_FALSE(hot_key.listening());
  ShortcutSettings::Get().SetRecording(false);
  EXPECT_TRUE(hot_key.listening());

  ASSERT_FALSE(FindLittleWindow());
  hot_key.OnKeyPressed(keys);
  EXPECT_TRUE(base::test::RunUntil([&] { return FindLittleWindow(); }));
  BrowserWindowInterface* little = FindLittleWindow();
  ASSERT_EQ(little->GetTabStripModel()->count(), 1);
  EXPECT_EQ(PageOf(little)->GetVisibleURL().host(), "new-tab-page");
  EXPECT_EQ(browser()->tab_strip_model()->count(), 1);
}

}  // namespace
}  // namespace compass
