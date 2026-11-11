// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_TEST_COMPASS_BROWSER_WINDOW_TEST_H_
#define COMPASS_TEST_COMPASS_BROWSER_WINDOW_TEST_H_

#include <memory>
#include <utility>

#include "base/compiler_specific.h"
#include "base/memory/raw_ptr.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/test/base/browser_with_test_window_test.h"

class BrowserWindow;
class GURL;
class Profile;

// BrowserWithTestWindowTest as it was until Chromium 157, which took the
// Browser out of it (upstream wants unit tests that need no Browser). Compass's
// window and space code is about Browsers, so its unit tests keep one: a
// normal window with a TestBrowserWindow, made in SetUp() and closed in
// TearDown(), and the helpers upstream removed, with the same behavior.
class CompassBrowserWindowTest : public BrowserWithTestWindowTest {
 public:
  // Takes BrowserWithTestWindowTest's task environment traits.
  template <typename... TaskEnvironmentTraits>
  NOINLINE explicit CompassBrowserWindowTest(TaskEnvironmentTraits&&... traits)
      : BrowserWithTestWindowTest(
            std::forward<TaskEnvironmentTraits>(traits)...) {}
  ~CompassBrowserWindowTest() override;

  void SetUp() override;
  void TearDown() override;

 protected:
  BrowserWindowInterface* browser() const { return browser_.get(); }
  BrowserWindow* window() const { return window_; }

  // Gives the test the window; browser() is null afterwards.
  std::unique_ptr<BrowserWindowInterface> release_browser();

  // Opens |url| in a new foreground tab at index 0 of |browser| and commits
  // the load. Creates a real WebContents.
  void AddTab(BrowserWindowInterface* browser, const GURL& url);

  // Navigates browser()'s active tab and commits.
  void NavigateAndCommitActiveTab(const GURL& url);

  // A window for |profile| with a TestBrowserWindow.
  std::unique_ptr<BrowserWindowInterface> CreateBrowser(
      Profile* profile,
      BrowserWindowInterface::Type browser_type,
      bool hosted_app);

 private:
  std::unique_ptr<BrowserWindowInterface> browser_;
  raw_ptr<BrowserWindow> window_ = nullptr;
};

#endif  // COMPASS_TEST_COMPASS_BROWSER_WINDOW_TEST_H_
