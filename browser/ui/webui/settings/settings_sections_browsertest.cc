// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/test/run_until.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/events/keycodes/dom/dom_code.h"
#include "ui/events/keycodes/dom/dom_key.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "url/gurl.h"

namespace compass {
namespace {

// Finds |selector| anywhere in the page, through every shadow root
// (chrome://settings nests the sections in settings-ui and settings-main).
constexpr char kDeepQuery[] = R"(
  function deepQuery(selector, root = document) {
    const found = root.querySelector(selector);
    if (found) {
      return found;
    }
    for (const element of root.querySelectorAll('*')) {
      if (element.shadowRoot) {
        const inside = deepQuery(selector, element.shadowRoot);
        if (inside) {
          return inside;
        }
      }
    }
    return null;
  }
)";

class SettingsSectionsBrowserTest : public InProcessBrowserTest {
 protected:
  content::WebContents* contents() {
    return browser()->tab_strip_model()->GetActiveWebContents();
  }

  void Open(const std::string& url) {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(browser(), GURL(url)));
  }

  // Resolves with what |expression| (using deepQuery()) gives once it's
  // truthy.
  content::EvalJsResult WaitFor(const std::string& expression) {
    return content::EvalJs(contents(), std::string(kDeepQuery) + R"(
      new Promise(resolve => {
        const check = () => {
          const value = ()" + expression + R"();
          if (value) {
            resolve(value);
          } else {
            setTimeout(check, 20);
          }
        };
        check();
      }))");
  }
};

// The menu of chrome://settings lists Compass's sections, and each one shows
// its element.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, MenuOpensEachSection) {
  Open("chrome://settings/");
  EXPECT_EQ(WaitFor("deepQuery('a#compassSync')?.getAttribute('href')"),
            "/sync");
  EXPECT_EQ(WaitFor("deepQuery('a#compassShortcuts')?.getAttribute('href')"),
            "/shortcuts");
  EXPECT_EQ(WaitFor("deepQuery('a#compassArcImport')?.getAttribute('href')"),
            "/importArc");
  EXPECT_EQ(WaitFor("deepQuery('a#compassClaude')?.getAttribute('href')"),
            "/claude");

  ASSERT_TRUE(content::ExecJs(contents(), std::string(kDeepQuery) +
                                              "deepQuery('a#compassShortcuts')"
                                              ".click();"));
  EXPECT_EQ(WaitFor("deepQuery('compass-shortcuts-page .compass-title')"
                    "?.textContent"),
            "Keyboard shortcuts");
  EXPECT_EQ(content::EvalJs(contents(), "location.pathname"), "/shortcuts");
}

// Upstream's "On startup" section is gone: its menu item is hidden and
// there's no view for it (docs/design/settings.md).
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, NoOnStartupSection) {
  Open("chrome://settings/");
  EXPECT_EQ(WaitFor("deepQuery('a#compassSync')?.getAttribute('href')"),
            "/sync");
  EXPECT_EQ(content::EvalJs(contents(), std::string(kDeepQuery) + R"(
              deepQuery('a#onStartup')?.hidden === true &&
                  !deepQuery('div#onStartup') &&
                  !deepQuery('settings-on-startup-page'))"),
            true);
}

// compass://settings/shortcuts lists the shortcuts and records keys as the
// standalone page does.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, ShortcutsSection) {
  Open("chrome://settings/shortcuts");
  // The space shortcuts are listed on every platform.
  EXPECT_EQ(WaitFor("deepQuery('compass-shortcuts-page li[data-id=\"space-1\"]"
                    " .label')?.textContent"),
            "Switch to space 1");

  // Clicking the keys records; Esc stops.
  ASSERT_TRUE(content::ExecJs(
      contents(), std::string(kDeepQuery) +
                      "deepQuery('compass-shortcuts-page "
                      "li[data-id=\"space-1\"] .keys').click();"));
  ASSERT_TRUE(base::test::RunUntil(
      [] { return ShortcutSettings::Get().recording(); }));
  content::SimulateKeyPress(contents(), ui::DomKey::ESCAPE,
                            ui::DomCode::ESCAPE, ui::VKEY_ESCAPE,
                            /*control=*/false, /*shift=*/false,
                            /*alt=*/false, /*command=*/false);
  ASSERT_TRUE(base::test::RunUntil(
      [] { return !ShortcutSettings::Get().recording(); }));
}

// compass://settings/sync shows sync's status (off in a new profile).
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, SyncSection) {
  Open("chrome://settings/sync");
  EXPECT_EQ(WaitFor("deepQuery('compass-sync-page #status')"
                    "?.textContent.startsWith('Sync is off')"),
            true);
  EXPECT_EQ(WaitFor("!deepQuery('compass-sync-page #off')?.hidden"), true);
}

// compass://settings/importArc looks for Arc and says what it found.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, ArcImportSection) {
  Open("chrome://settings/importArc");
  EXPECT_EQ(WaitFor("deepQuery('compass-arc-import-page #loading')?.hidden"),
            true);
}

// compass://settings/claude asks for a key in a new profile.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, ClaudeSection) {
  Open("chrome://settings/claude");
  EXPECT_EQ(WaitFor("deepQuery('compass-claude-page .compass-title')"
                    "?.textContent"),
            "Claude");
  EXPECT_EQ(WaitFor("deepQuery('compass-claude-page #keySaved')?.hidden"),
            true);
  EXPECT_EQ(WaitFor("deepQuery('compass-claude-page #model')?.value"),
            "claude-opus-5-5");
}

// Settings' search keeps a Compass section that has the words.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, SearchFindsASection) {
  Open("chrome://settings/");
  EXPECT_EQ(
      content::EvalJs(contents(), std::string(kDeepQuery) + R"(
        (async () => {
          await customElements.whenDefined('compass-sync-page');
          const page = document.createElement('compass-sync-page');
          document.body.append(page);
          const found = await page.searchContents('recovery code');
          const missing = await page.searchContents('zzzz no such words');
          page.remove();
          return found.matchCount > 0 && missing.matchCount === 0;
        })())"),
      true);
}

}  // namespace
}  // namespace compass
