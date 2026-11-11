// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/test/compass_browser_window_test.h"

#include <utility>

#include "base/run_loop.h"
#include "chrome/browser/ui/browser_window/public/browser_window_features.h"
#include "chrome/browser/ui/browser_window/public/create_browser_window.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/test_browser_window.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

CompassBrowserWindowTest::~CompassBrowserWindowTest() = default;

void CompassBrowserWindowTest::SetUp() {
  BrowserWithTestWindowTest::SetUp();
  auto window = std::make_unique<TestBrowserWindow>();
  window_ = window.get();
  BrowserWindowCreateParams params(profile(), true);
  params.window = window.release();
  browser_ = DeprecatedCreateOwnedBrowserWindowForTesting(std::move(params));
}

void CompassBrowserWindowTest::TearDown() {
  // As upstream did before 157: let pending tasks finish, then close the
  // window before the profile goes.
  base::RunLoop().RunUntilIdle();
  window_ = nullptr;
  if (browser_) {
    browser_->GetTabStripModel()->CloseAllTabs();
    browser_->GetFeatures().TearDownPreBrowserWindowDestruction();
    browser_.reset();
  }
  BrowserWithTestWindowTest::TearDown();
}

std::unique_ptr<BrowserWindowInterface>
CompassBrowserWindowTest::release_browser() {
  window_ = nullptr;
  return std::move(browser_);
}

void CompassBrowserWindowTest::AddTab(BrowserWindowInterface* browser,
                                      const GURL& url) {
  NavigateParams params(browser, url, ui::PAGE_TRANSITION_TYPED);
  params.tabstrip_index = 0;
  params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  Navigate(&params);
  CommitPendingLoad(&params.navigated_or_inserted_contents->GetController());
}

void CompassBrowserWindowTest::NavigateAndCommitActiveTab(const GURL& url) {
  NavigateAndCommit(browser()->GetTabStripModel()->GetActiveWebContents(), url);
}

std::unique_ptr<BrowserWindowInterface> CompassBrowserWindowTest::CreateBrowser(
    Profile* profile,
    BrowserWindowInterface::Type browser_type,
    bool hosted_app) {
  BrowserWindowCreateParams params(profile, true);
  if (hosted_app) {
    params = BrowserWindowCreateParams::CreateForApp(
        "Test", /*trusted_source=*/true, /*window_bounds=*/gfx::Rect(), profile,
        /*user_gesture=*/true);
  } else if (browser_type == BrowserWindowInterface::Type::TYPE_DEVTOOLS) {
    params = BrowserWindowCreateParams::CreateForDevTools(profile);
  } else {
    params.type = browser_type;
  }
  params.window = std::make_unique<TestBrowserWindow>().release();
  return DeprecatedCreateOwnedBrowserWindowForTesting(std::move(params));
}
