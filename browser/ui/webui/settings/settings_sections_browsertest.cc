// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/memory/raw_ptr.h"
#include "base/test/run_until.h"
#include "base/uuid.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/boosts/boost_store.h"
#include "compass/browser/dev_mode/dev_mode.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/shortcuts/shortcut_settings.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/views/settings_window/settings_window.h"
#include "compass/browser/window_sync/space_service.h"
#include "components/prefs/pref_service.h"
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

// Finds |selector| anywhere in the page, through every shadow root.
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
  content::WebContents* contents() { return contents_; }

  // Opens settings at |sub_page| as the browser does
  // (chrome::ShowSettingsSubPage()): Compass's sections are in the settings
  // window (docs/design/settings-window.md).
  void OpenSection(const std::string& sub_page) {
    chrome::ShowSettingsSubPage(browser(), sub_page);
    SettingsWindow* window =
        SettingsWindow::FromProfile(browser()->GetProfile());
    ASSERT_TRUE(window);
    contents_ = window->web_contents();
    ASSERT_TRUE(content::WaitForLoadStop(contents_));
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

 private:
  raw_ptr<content::WebContents, DanglingUntriaged> contents_ = nullptr;
};

// compass://settings/shortcuts lists the shortcuts and records keys as the
// standalone page does.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, ShortcutsSection) {
  OpenSection("shortcuts");
  // The space shortcuts are listed on every platform.
  EXPECT_EQ(WaitFor("deepQuery('compass-shortcuts-page li[data-id=\"space-1\"]"
                    " .label')?.textContent"),
            "Switch to space 1");

  // Clicking the keys records; Esc stops.
  ASSERT_TRUE(content::ExecJs(contents(),
                              std::string(kDeepQuery) +
                                  "deepQuery('compass-shortcuts-page "
                                  "li[data-id=\"space-1\"] .keys').click();"));
  ASSERT_TRUE(
      base::test::RunUntil([] { return ShortcutSettings::Get().recording(); }));
  content::SimulateKeyPress(contents(), ui::DomKey::ESCAPE, ui::DomCode::ESCAPE,
                            ui::VKEY_ESCAPE,
                            /*control=*/false, /*shift=*/false,
                            /*alt=*/false, /*command=*/false);
  ASSERT_TRUE(base::test::RunUntil(
      [] { return !ShortcutSettings::Get().recording(); }));
}

// compass://settings/links (docs/design/link-routing.md): "?add=" opens the
// form for a domain, saving makes the rule, a rule whose space is gone is
// listed as broken, and Delete removes one.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, LinksSection) {
  SpaceList& spaces = SpaceService::Get(browser()->GetProfile())->space_list();
  SpaceModel* work = spaces.AddSpace(spaces.spaces().size(), u"Work");
  ASSERT_TRUE(spaces.SetLinkRule("gitlab.com", base::Uuid::GenerateRandomV4()));

  OpenSection("links?add=www.GitHub.com");
  EXPECT_EQ(WaitFor("deepQuery('compass-links-page .compass-title')"
                    "?.textContent"),
            "Links from Other Apps");
  EXPECT_EQ(WaitFor("deepQuery('compass-links-page li[data-domain="
                    "\"gitlab.com\"] .links-broken')?.textContent"),
            "Space deleted: opens in a little window");
  EXPECT_EQ(WaitFor("deepQuery('compass-links-page #linkRuleForm')?.hidden === "
                    "false && "
                    "deepQuery('compass-links-page #linkDomain')?.value"),
            "www.GitHub.com");

  ASSERT_TRUE(content::ExecJs(
      contents(), std::string(kDeepQuery) +
                      "deepQuery('compass-links-page #linkSpace').value = '" +
                      work->id().AsLowercaseString() +
                      "'; deepQuery('compass-links-page #saveLinkRule')"
                      ".click();"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return spaces.link_rules().Find("github.com"); }));
  EXPECT_EQ(*spaces.link_rules().Find("github.com"), work->id());
  EXPECT_EQ(WaitFor("deepQuery('compass-links-page li[data-domain="
                    "\"github.com\"]')?.textContent.includes('Work')"),
            true);

  ASSERT_TRUE(content::ExecJs(
      contents(), std::string(kDeepQuery) +
                      "deepQuery('compass-links-page li[data-domain="
                      "\"gitlab.com\"] button:last-child').click();"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return !spaces.link_rules().Find("gitlab.com"); }));
}

// compass://settings/devMode (docs/design/dev-mode.md): adding a site keeps
// its host (no port), one that isn't a site says so, and Remove removes one.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, DevModeSection) {
  PrefService* prefs = browser()->GetProfile()->GetPrefs();
  ASSERT_TRUE(dev_mode::AddHost(prefs, "example.com"));

  OpenSection("devMode");
  EXPECT_EQ(WaitFor("deepQuery('compass-dev-mode-page .compass-title')"
                    "?.textContent"),
            "Development Mode");
  EXPECT_EQ(WaitFor("!!deepQuery('compass-dev-mode-page li[data-host="
                    "\"example.com\"]')"),
            true);

  ASSERT_TRUE(content::ExecJs(
      contents(), std::string(kDeepQuery) +
                      "deepQuery('compass-dev-mode-page #devModeHost').value ="
                      " 'http://localhost:3000/app';"
                      "deepQuery('compass-dev-mode-page #addDevModeHost')"
                      ".click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return dev_mode::IsEnabledFor(prefs, GURL("http://localhost:5173/"));
  }));
  EXPECT_EQ(WaitFor("!!deepQuery('compass-dev-mode-page li[data-host="
                    "\"localhost\"]')"),
            true);

  ASSERT_TRUE(content::ExecJs(
      contents(), std::string(kDeepQuery) +
                      "deepQuery('compass-dev-mode-page #devModeHost').value ="
                      " 'chrome://settings';"
                      "deepQuery('compass-dev-mode-page #addDevModeHost')"
                      ".click();"));
  EXPECT_EQ(WaitFor("deepQuery('compass-dev-mode-page #devModeNote')?.hidden "
                    "=== false"),
            true);

  ASSERT_TRUE(content::ExecJs(
      contents(), std::string(kDeepQuery) +
                      "deepQuery('compass-dev-mode-page li[data-host="
                      "\"example.com\"] button').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !dev_mode::IsEnabledFor(prefs, GURL("https://example.com/"));
  }));
}

// compass://settings/boosts (docs/design/boosts.md): "?add=" opens the
// editor on a host, saving makes the Boost (the user's), a Boost Claude made
// has its badge, the switch turns one off, and Delete removes one.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, BoostsSection) {
  boosts::BoostStore* store = boosts::BoostStore::Get(browser()->GetProfile());
  ASSERT_TRUE(base::test::RunUntil([&] { return store->loaded(); }));
  boosts::Boost claudes;
  claudes.match = "gitlab.com";
  claudes.css = "a {}";
  claudes.author = boosts::kAuthorClaude;
  ASSERT_TRUE(store->Save(claudes).has_value());

  OpenSection("boosts?add=www.GitHub.com");
  EXPECT_EQ(WaitFor("deepQuery('compass-boosts-page .compass-title')"
                    "?.textContent"),
            "Boosts");
  EXPECT_EQ(WaitFor("deepQuery('compass-boosts-page .boosts-badge')"
                    "?.textContent"),
            "by AI");
  EXPECT_EQ(WaitFor("deepQuery('compass-boosts-page #boostForm')?.hidden === "
                    "false && "
                    "deepQuery('compass-boosts-page #boostMatch')?.value"),
            "www.GitHub.com");

  ASSERT_TRUE(content::ExecJs(
      contents(), std::string(kDeepQuery) +
                      "deepQuery('compass-boosts-page #boostCss').value = "
                      "'header { display: none }';"
                      "deepQuery('compass-boosts-page #saveBoost').click();"));
  ASSERT_TRUE(
      base::test::RunUntil([&] { return store->boosts().size() == 2u; }));
  const boosts::Boost& mine = store->boosts()[1];
  EXPECT_EQ(mine.match, "github.com");
  EXPECT_EQ(mine.author, boosts::kAuthorUser);
  EXPECT_TRUE(mine.enabled);
  const std::string id = mine.id;
  const std::string row =
      "deepQuery('compass-boosts-page li[data-id=\"" + id + "\"]";
  EXPECT_EQ(WaitFor(row + "')?.textContent.includes('CSS')"), true);

  ASSERT_TRUE(content::ExecJs(
      contents(), std::string(kDeepQuery) + row + " input').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] { return !store->Find(id)->enabled; }));

  ASSERT_TRUE(content::ExecJs(contents(), std::string(kDeepQuery) + row +
                                              " button:last-child').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] { return !store->Find(id); }));
}

// compass://settings/sync shows sync's status (off in a new profile).
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, SyncSection) {
  OpenSection("sync");
  EXPECT_EQ(WaitFor("deepQuery('compass-sync-page #status')"
                    "?.textContent.startsWith('Sync is off')"),
            true);
  EXPECT_EQ(WaitFor("!deepQuery('compass-sync-page #off')?.hidden"), true);
}

// compass://settings/importArc looks for other browsers and Arc, and says
// what it found.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, ArcImportSection) {
  OpenSection("importArc");
  EXPECT_EQ(WaitFor("deepQuery('compass-arc-import-page #loading')?.hidden"),
            true);
  EXPECT_EQ(WaitFor("deepQuery('compass-arc-import-page #browsersLoading')"
                    "?.hidden"),
            true);
  // The list first.
  EXPECT_EQ(WaitFor("deepQuery('compass-arc-import-page #listView') && "
                    "!deepQuery('compass-arc-import-page #listView').hidden"),
            true);
}

// compass://settings/claude asks for a key in a new profile.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, ClaudeSection) {
  OpenSection("claude");
  EXPECT_EQ(WaitFor("deepQuery('compass-claude-page .compass-title')"
                    "?.textContent"),
            "AI");
  EXPECT_EQ(WaitFor("deepQuery('compass-claude-page #keySaved')?.hidden"),
            true);
  EXPECT_EQ(WaitFor("deepQuery('compass-claude-page #model')?.value"),
            "claude-opus-5-5");
}

// compass://settings/plugins (docs/design/settings.md, "Plugins"): the
// built-ins with what they do, and a preview's switches write its settings.
IN_PROC_BROWSER_TEST_F(SettingsSectionsBrowserTest, PluginsSection) {
  PrefService* prefs = browser()->GetProfile()->GetPrefs();
  OpenSection("plugins");
  EXPECT_EQ(WaitFor("deepQuery('compass-plugins-page .compass-title')"
                    "?.textContent"),
            "Plugins");
  // GitHub fills live folders, previews tabs and counts on them; RSS only
  // fills folders.
  EXPECT_EQ(WaitFor("[...deepQuery('compass-plugins-page "
                    "li[data-id=\"compass.github\"]').querySelectorAll("
                    "'.plugin-chip')].map(c => c.textContent).join(',')"),
            "Live folder,Tab preview,Badge");
  EXPECT_EQ(WaitFor("[...deepQuery('compass-plugins-page "
                    "li[data-id=\"compass.rss\"]').querySelectorAll("
                    "'.plugin-chip')].map(c => c.textContent).join(',')"),
            "Live folder");
  // Gmail only previews tabs and counts on them.
  EXPECT_EQ(WaitFor("[...deepQuery('compass-plugins-page "
                    "li[data-id=\"compass.gmail\"]').querySelectorAll("
                    "'.plugin-chip')].map(c => c.textContent).join(',')"),
            "Tab preview,Badge");

  // Built-ins' previews are on; turning GitHub's off is kept.
  ASSERT_TRUE(content::ExecJs(
      contents(), std::string(kDeepQuery) +
                      "deepQuery('compass-plugins-page "
                      "#plugin-toggle-preview-compass\\\\.github').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    const base::DictValue* github =
        prefs->GetDict(prefs::kTabPreviewPlugins).FindDict("compass.github");
    return github && github->FindBool("enabled") == false;
  }));

  // A built-in's session is on without asking; turning it off is kept, and
  // on again is granted for the hosts it lists.
  const std::string session_toggle =
      std::string(kDeepQuery) +
      "deepQuery('compass-plugins-page "
      "#plugin-toggle-session-compass\\\\.github')";
  EXPECT_EQ(true, content::EvalJs(contents(), session_toggle + ".checked"));
  ASSERT_TRUE(content::ExecJs(contents(), session_toggle + ".click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    const base::DictValue* github =
        prefs->GetDict(prefs::kTabPreviewPlugins).FindDict("compass.github");
    return github && github->FindBool("session_granted") == false;
  }));
  ASSERT_TRUE(content::ExecJs(contents(), session_toggle + ".click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    const base::DictValue* github =
        prefs->GetDict(prefs::kTabPreviewPlugins).FindDict("compass.github");
    const base::ListValue* hosts =
        github ? github->FindList("session_hosts") : nullptr;
    return github && github->FindBool("session_granted") == true && hosts &&
           hosts->size() == 1 &&
           (*hosts)[0] == base::Value("avatars.githubusercontent.com");
  }));

  // Hover cards' previews, all at once.
  ASSERT_TRUE(content::ExecJs(
      contents(), std::string(kDeepQuery) + "deepQuery('compass-plugins-page "
                                            "#tabPreviewsEnabled').click();"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return !prefs->GetBoolean(prefs::kTabPreviewsEnabled); }));
}

}  // namespace
}  // namespace compass
