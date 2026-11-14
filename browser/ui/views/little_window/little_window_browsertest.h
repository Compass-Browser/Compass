// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_BROWSERTEST_H_
#define COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_BROWSERTEST_H_

#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/profile_browser_collection.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "compass/browser/ui/views/little_window/little_window.h"
#include "content/public/browser/web_contents.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {

// The little window tests' fixture: every platform's tests
// (little_window_browsertest.cc) and macOS's own
// (little_window_browsertest_mac.mm) are one suite.
class LittleWindowBrowserTest : public InProcessBrowserTest {
 public:
  // Starts the test server (little_window_browsertest.cc).
  void SetUpOnMainThread() override;

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

}  // namespace compass

#endif  // COMPASS_BROWSER_UI_VIEWS_LITTLE_WINDOW_LITTLE_WINDOW_BROWSERTEST_H_
