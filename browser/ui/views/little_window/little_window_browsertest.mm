// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/little_window/little_window.h"

#import <Cocoa/Cocoa.h>

#include <vector>

#include "base/strings/sys_string_conversions.h"
#include "base/test/run_until.h"
#include "chrome/app/chrome_command_ids.h"
#import "chrome/browser/app_controller_mac.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/startup/startup_tab.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

class LittleWindowBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(embedded_test_server()->Start());
  }

 protected:
  // The profile's little window, or null.
  BrowserWindowInterface* FindLittleWindow() {
    BrowserWindowInterface* found = nullptr;
    ProfileBrowserCollection::GetForProfile(browser()->GetProfile())
        ->ForEach([&](BrowserWindowInterface* window) {
          if (IsLittleWindow(window)) {
            found = window;
          }
          return !found;
        });
    return found;
  }

  content::WebContents* PageOf(BrowserWindowInterface* window) {
    return window->GetTabStripModel()->GetActiveWebContents();
  }
};

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

// Anything but web pages and files opens as upstream does.
IN_PROC_BROWSER_TEST_F(LittleWindowBrowserTest, OnlyWebPagesAndFiles) {
  std::vector<StartupTab> tabs;
  tabs.emplace_back(embedded_test_server()->GetURL("/title1.html"), false);
  tabs.emplace_back(GURL("chrome://version/"), false);
  EXPECT_FALSE(OpenInLittleWindows(tabs, browser()->GetProfile()));
  EXPECT_FALSE(FindLittleWindow());
}

// Cmd+O moves the page (the same WebContents) into the window as the active
// tab, and the little window closes.
IN_PROC_BROWSER_TEST_F(LittleWindowBrowserTest, OpenInSpaceMovesThePage) {
  BrowserWindowInterface* little = OpenLittleWindow(
      browser()->GetProfile(), embedded_test_server()->GetURL("/title1.html"));
  ASSERT_TRUE(little);
  content::WebContents* page = PageOf(little);
  ASSERT_TRUE(content::WaitForLoadStop(page));

  chrome::ExecuteCommand(little, IDC_OPEN_FILE);
  EXPECT_EQ(browser()->tab_strip_model()->count(), 2);
  EXPECT_EQ(browser()->tab_strip_model()->GetActiveWebContents(), page);
  EXPECT_TRUE(base::test::RunUntil([&] { return !FindLittleWindow(); }));
}

// Back is there, and follows the page's history.
IN_PROC_BROWSER_TEST_F(LittleWindowBrowserTest, BackFollowsHistory) {
  BrowserWindowInterface* little = OpenLittleWindow(
      browser()->GetProfile(), embedded_test_server()->GetURL("/title1.html"));
  ASSERT_TRUE(little);
  content::WebContents* page = PageOf(little);
  ASSERT_TRUE(content::WaitForLoadStop(page));
  EXPECT_FALSE(chrome::IsCommandEnabled(little, IDC_BACK));

  ASSERT_TRUE(content::NavigateToURL(
      page, embedded_test_server()->GetURL("/title2.html")));
  EXPECT_TRUE(chrome::IsCommandEnabled(little, IDC_BACK));
  chrome::ExecuteCommand(little, IDC_BACK);
  ASSERT_TRUE(content::WaitForLoadStop(page));
  EXPECT_EQ(page->GetLastCommittedURL().path(), "/title1.html");
}

}  // namespace
}  // namespace compass
