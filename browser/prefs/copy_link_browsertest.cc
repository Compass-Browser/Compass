// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// "Remove tracking from copied links" and the licenses of its lists, in the
// real settings pages (docs/design/shortcuts.md "Clean links").

#include <string>

#include "base/test/run_until.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/ui/views/settings_window/settings_window.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

class CopyLinkSettingsBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }
};

IN_PROC_BROWSER_TEST_F(CopyLinkSettingsBrowserTest, LicensesPage) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://compass-settings/licenses.html")));
  EXPECT_EQ(
      content::EvalJs(contents(), "document.querySelectorAll('pre').length"),
      2);
  const std::string text =
      content::EvalJs(contents(), "document.body.innerText").ExtractString();
  EXPECT_NE(text.find("Mozilla Public License Version 2.0"), std::string::npos);
  EXPECT_NE(text.find("GNU LESSER GENERAL PUBLIC LICENSE"), std::string::npos);
  EXPECT_NE(text.find("Firefox's strip-on-share list"), std::string::npos);
}

// Settings' About Compass (the settings window's Advanced pane,
// docs/design/settings-window.md) links to the licenses.
IN_PROC_BROWSER_TEST_F(CopyLinkSettingsBrowserTest, AboutLinksToLicenses) {
  chrome::ShowSettingsSubPage(browser(), "help");
  SettingsWindow* window = SettingsWindow::FromProfile(browser()->GetProfile());
  ASSERT_TRUE(window);
  ASSERT_TRUE(content::WaitForLoadStop(window->web_contents()));
  EXPECT_EQ(content::EvalJs(window->web_contents(), R"(
      new Promise(resolve => {
        const check = () => {
          const row = document.querySelector('[data-row="licenses"]');
          row ? resolve(row.textContent.trim()) : setTimeout(check, 20);
        };
        check();
      }))"),
            "Open-source licenses");
}

// The settings window's Privacy pane has the switch, which turns the pref
// off.
IN_PROC_BROWSER_TEST_F(CopyLinkSettingsBrowserTest, ToggleInPrivacy) {
  PrefService* prefs = browser()->GetProfile()->GetPrefs();
  ASSERT_TRUE(prefs->GetBoolean(prefs::kRemoveTrackingFromCopiedLinks));
  chrome::ShowSettingsSubPage(browser(), "cookies");
  SettingsWindow* window = SettingsWindow::FromProfile(browser()->GetProfile());
  ASSERT_TRUE(window);
  ASSERT_TRUE(content::WaitForLoadStop(window->web_contents()));
  EXPECT_EQ(content::EvalJs(window->web_contents(), R"(
      new Promise(resolve => {
        const check = () => {
          const row = document.querySelector('[data-row="cleanLinks"]');
          const input = row?.querySelector('input');
          if (input && input.checked) {
            input.click();
            resolve(row.querySelector('.row-label').textContent);
          } else {
            setTimeout(check, 20);
          }
        };
        check();
      }))"),
            "Remove tracking from copied links");
  EXPECT_TRUE(base::test::RunUntil([&] {
    return !prefs->GetBoolean(prefs::kRemoveTrackingFromCopiedLinks);
  }));
}

}  // namespace
}  // namespace compass
