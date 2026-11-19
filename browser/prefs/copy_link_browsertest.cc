// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// "Remove tracking from copied links" and the licenses of its lists, in the
// real settings pages (docs/design/shortcuts.md "Clean links").

#include <string>

#include "base/test/run_until.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass {
namespace {

// Finds |selector| through every shadow root, waiting for settings' lazy
// rendering; resolves with the element's label attribute, else its text, or
// null.
constexpr char kDeepFind[] = R"(
  (selector) => new Promise(resolve => {
    const find = root => {
      const hit = root.querySelector(selector);
      if (hit) return hit;
      for (const el of root.querySelectorAll('*')) {
        if (el.shadowRoot) {
          const inner = find(el.shadowRoot);
          if (inner) return inner;
        }
      }
      return null;
    };
    let tries = 0;
    const look = () => {
      const hit = find(document);
      if (hit || ++tries > 100) {
        resolve(hit ? (hit.getAttribute('label') ??
                       hit.textContent.replace(/\s+/g, ' ').trim())
                    : null);
        return;
      }
      setTimeout(look, 100);
    };
    look();
  }))";

class CopyLinkSettingsBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  content::EvalJsResult Find(const std::string& selector) {
    return content::EvalJs(contents(), std::string("(") + kDeepFind +
                                           ")('" + selector + "')");
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

IN_PROC_BROWSER_TEST_F(CopyLinkSettingsBrowserTest, AboutPageLinksToLicenses) {
  ASSERT_TRUE(
      ui_test_utils::NavigateToURL(browser(), GURL("chrome://settings/help")));
  EXPECT_EQ(Find("#compassLicenses"),
            "third-party files under their own licenses");
}

IN_PROC_BROWSER_TEST_F(CopyLinkSettingsBrowserTest, ToggleOnCookiesPage) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(),
                                           GURL("chrome://settings/cookies")));
  EXPECT_EQ(Find("#compassRemoveTrackingFromCopiedLinks"),
            "Remove tracking from copied links");

  // Clicking it turns the pref off.
  PrefService* prefs = browser()->GetProfile()->GetPrefs();
  ASSERT_TRUE(prefs->GetBoolean(prefs::kRemoveTrackingFromCopiedLinks));
  ASSERT_TRUE(content::ExecJs(
      contents(),
      std::string("(") + kDeepFind +
          ")('#compassRemoveTrackingFromCopiedLinks').then(() => {"
          "  const find = root => root.querySelector("
          "      '#compassRemoveTrackingFromCopiedLinks') ||"
          "    [...root.querySelectorAll('*')].filter(e => e.shadowRoot)"
          "      .map(e => find(e.shadowRoot)).find(Boolean);"
          "  find(document).click();"
          "})"));
  EXPECT_TRUE(base::test::RunUntil([&] {
    return !prefs->GetBoolean(prefs::kRemoveTrackingFromCopiedLinks);
  }));
}

}  // namespace
}  // namespace compass
