// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/settings_window/settings_window.h"

#include <string>

#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/run_until.h"
#include "chrome/browser/autofill/personal_data_manager_factory.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/search_test_utils.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/autofill/core/browser/data_manager/addresses/address_data_manager.h"
#include "components/autofill/core/browser/data_manager/personal_data_manager.h"
#include "components/autofill/core/browser/data_model/addresses/autofill_profile.h"
#include "components/autofill/core/browser/field_types.h"
#include "components/browsing_data/core/pref_names.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/keyed_service/core/service_access_type.h"
#include "components/language/core/browser/pref_names.h"
#include "components/password_manager/core/browser/password_store/password_store_interface.h"
#include "components/password_manager/core/browser/password_store/stored_credential.h"
#include "components/password_manager/core/common/password_manager_pref_names.h"
#include "components/performance_manager/public/user_tuning/prefs.h"
#include "components/prefs/pref_service.h"
#include "components/safe_browsing/core/common/safe_browsing_prefs.h"
#include "components/search_engines/template_url_service.h"
#include "components/translate/core/browser/translate_prefs.h"
#include "content/public/browser/web_contents.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace compass {
namespace {

class SettingsWindowBrowserTest : public InProcessBrowserTest {
 protected:
  Profile* profile() { return browser()->GetProfile(); }

  SettingsWindow* window() { return SettingsWindow::FromProfile(profile()); }

  int tab_count() { return browser()->GetTabStripModel()->count(); }

  // Resolves with what |expression| gives in the window's page once it's
  // truthy.
  content::EvalJsResult WaitFor(const std::string& expression) {
    return content::EvalJs(window()->web_contents(), R"(
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

IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, PathsTheWindowHas) {
  EXPECT_EQ(SettingsWindowPathFor(""), "");
  EXPECT_EQ(SettingsWindowPathFor("boosts?add=example.com"),
            "boosts?add=example.com");
  EXPECT_EQ(SettingsWindowPathFor("/sync"), "sync");
  EXPECT_EQ(SettingsWindowPathFor("claude"), "ai");
  EXPECT_EQ(SettingsWindowPathFor("devMode"), "devMode");
  // Upstream's sections stay upstream's for now.
  EXPECT_EQ(SettingsWindowPathFor("searchEngines"), "general?view=engines");
  EXPECT_EQ(SettingsWindowPathFor("languages"), "general?view=languages");
  EXPECT_EQ(SettingsWindowPathFor("ai"), std::nullopt);
  EXPECT_EQ(SettingsWindowPathFor("safetyCheck"), std::nullopt);
  EXPECT_EQ(SettingsWindowPathFor("payments"), std::nullopt);
  EXPECT_EQ(SettingsWindowPathFor("passwords"), "passwords");
  EXPECT_EQ(SettingsWindowPathFor("help"), "advanced?focus=about");
  EXPECT_EQ(SettingsWindowPathFor("reset"), "advanced?focus=reset");
  // Upstream's privacy sections are the Privacy pane's.
  EXPECT_EQ(SettingsWindowPathFor("privacy"), "privacy");
  EXPECT_EQ(SettingsWindowPathFor("security"), "privacy?focus=security");
  EXPECT_EQ(SettingsWindowPathFor("clearBrowserData"), "privacy?view=clear");
  EXPECT_EQ(SettingsWindowPathFor(
                "content/siteDetails?site=https%3A%2F%2Fexample.com"),
            "privacy?site=https%3A%2F%2Fexample.com");
  EXPECT_EQ(SettingsWindowPathFor("content/location"),
            "privacy?view=permissions");
}

// Settings (IDC_OPTIONS, Cmd+,) open the window, not a tab; opening them again
// shows the same window at the new pane.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, ShowSettingsOpensTheWindow) {
  const int tabs = tab_count();
  chrome::ShowSettings(browser());
  ASSERT_TRUE(window());
  EXPECT_EQ(tab_count(), tabs);
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  EXPECT_EQ(WaitFor("document.querySelector('#tabs .tab') && "
                    "document.querySelectorAll('#tabs .tab').length"),
            10);

  SettingsWindow* first = window();
  chrome::ShowSettingsSubPage(browser(), "boosts");
  EXPECT_EQ(window(), first);
  EXPECT_EQ(WaitFor("location.pathname === '/boosts' && "
                    "document.querySelector('compass-boosts-page') && "
                    "document.querySelector('#title').textContent"),
            "Boosts");
  EXPECT_EQ(tab_count(), tabs);
}

// The toolbar's tabs switch panes; a Compass section's sub-page opens its
// pane at it.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, ToolbarSwitchesPanes) {
  chrome::ShowSettingsSubPage(browser(), "sync");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  EXPECT_EQ(WaitFor("document.querySelector('compass-sync-page') && "
                    "document.querySelector('.tab[aria-selected=true]')"
                    ".dataset.pane"),
            "profiles");

  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('.tab[data-pane=\"shortcuts\"]').click();"));
  EXPECT_EQ(WaitFor("document.querySelector('compass-shortcuts-page') && "
                    "location.pathname"),
            "/shortcuts");

  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('.tab[data-pane=\"ai\"]').click();"));
  EXPECT_EQ(WaitFor("document.querySelector('compass-claude-page') && "
                    "document.querySelector('#title').textContent"),
            "AI");
}

// Search finds rows and sections across panes.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, SearchFindsRows) {
  chrome::ShowSettings(browser());
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  ASSERT_TRUE(
      WaitFor("!!document.querySelector('#searchInput')").ExtractBool());
  ASSERT_TRUE(content::ExecJs(window()->web_contents(), R"(
      const input = document.querySelector('#searchInput');
      input.value = 'cookies';
      input.dispatchEvent(new Event('input'));)"));
  EXPECT_EQ(WaitFor("[...document.querySelectorAll('#results .row-label')]"
                    ".map(r => r.textContent).join(',')"),
            "Clear browsing data,Third-party cookies");
}

// A tab loading Compass's settings opens the window instead, and a tab opened
// only for it closes.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, SettingsURLOpensTheWindow) {
  const int tabs = tab_count();
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), GURL("chrome://settings/links?add=example.com"),
      WindowOpenDisposition::NEW_FOREGROUND_TAB,
      ui_test_utils::BROWSER_TEST_NO_WAIT);
  ASSERT_TRUE(base::test::RunUntil([&] { return !!window(); }));
  ASSERT_TRUE(base::test::RunUntil([&] { return tab_count() == tabs; }));
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  EXPECT_EQ(WaitFor("document.querySelector('compass-links-page "
                    "#linkDomain')?.value"),
            "example.com");
}

// Upstream's sections the window doesn't have yet still open
// chrome://settings in a tab.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, UpstreamSectionsStayInTabs) {
  chrome::ShowSettingsSubPage(browser(), "payments");
  EXPECT_FALSE(window());
  content::WebContents* tab =
      browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(tab));
  EXPECT_EQ(tab->GetLastCommittedURL(), GURL("chrome://settings/payments"));
}

// Closing the window lets settings open a new one.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, ClosesAndOpensAgain) {
  chrome::ShowSettings(browser());
  ASSERT_TRUE(window());
  SettingsWindow* first = window();
  first->Close();
  EXPECT_FALSE(window());
  chrome::ShowSettings(browser());
  EXPECT_TRUE(window());
  EXPECT_NE(window(), first);
}

// General (phase 2): a switch writes its pref, a menu its own, and the pane
// follows a pref changed elsewhere.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, GeneralPaneWritesPrefs) {
  PrefService* prefs = profile()->GetPrefs();
  prefs->SetBoolean(::prefs::kPromptForDownload, false);
  chrome::ShowSettingsSubPage(browser(), "general");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  const std::string prompt =
      "document.querySelector('input[data-pref=\"download.prompt_for_download"
      "\"]')";
  EXPECT_EQ(WaitFor(prompt + " && " + prompt + ".checked === false"), true);

  ASSERT_TRUE(content::ExecJs(window()->web_contents(), prompt + ".click();"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return prefs->GetBoolean(::prefs::kPromptForDownload); }));

  ASSERT_TRUE(content::ExecJs(window()->web_contents(), R"(
      const size = document.querySelector(
          'select[data-pref="webkit.webprefs.default_font_size"]');
      size.value = '20';
      size.dispatchEvent(new Event('change'));)"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return prefs->GetInteger(::prefs::kWebKitDefaultFontSize) == 20;
  }));

  // Changed elsewhere: the switch follows.
  prefs->SetBoolean(::prefs::kPromptForDownload, false);
  EXPECT_EQ(WaitFor(prompt + ".checked === false"), true);
}

// General: the search engine menu lists the engines and sets the default.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, GeneralPaneSearchEngine) {
  TemplateURLService* service =
      TemplateURLServiceFactory::GetForProfile(profile());
  search_test_utils::WaitForTemplateURLServiceToLoad(service);
  chrome::ShowSettingsSubPage(browser(), "general");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  const std::string menu =
      "document.querySelector('select[aria-label=\"Search engine\"]')";
  ASSERT_TRUE(
      WaitFor(menu + " && " + menu + ".options.length > 1").ExtractBool());

  // The first engine that isn't the default.
  const std::string other =
      content::EvalJs(window()->web_contents(),
                      "[..." + menu + ".options].find(o => !o.selected).value")
          .ExtractString();
  ASSERT_TRUE(content::ExecJs(window()->web_contents(),
                              menu + ".value = '" + other + "';" + menu +
                                  ".dispatchEvent(new Event('change'));"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return base::NumberToString(
               service->GetDefaultSearchProvider()->id().value()) == other;
  }));
}

// Profiles: the profile's name is saved when the field is left.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, ProfileName) {
  chrome::ShowSettingsSubPage(browser(), "profiles");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  const std::string field =
      "document.querySelector('input[aria-label=\"Profile name\"]')";
  ASSERT_TRUE(WaitFor(field + " && " + field + ".value").ExtractString() != "");
  ASSERT_TRUE(content::ExecJs(window()->web_contents(),
                              field + ".value = 'Work';" + field +
                                  ".dispatchEvent(new Event('change'));"));
  ProfileAttributesEntry* entry =
      g_browser_process->profile_manager()
          ->GetProfileAttributesStorage()
          .GetProfileAttributesWithPath(profile()->GetPath());
  ASSERT_TRUE(entry);
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return entry->GetLocalProfileName() == u"Work"; }));
  EXPECT_EQ(WaitFor("document.querySelectorAll('.avatar').length > 0"), true);
}

// Privacy (phase 3): page info's "Site settings" lands on the site, whose
// permissions change and reset there.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, PrivacySitePermissions) {
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile());
  const GURL site("https://example.com/");
  map->SetContentSettingDefaultScope(site, site,
                                     ContentSettingsType::MEDIASTREAM_CAMERA,
                                     CONTENT_SETTING_BLOCK);
  // Page info's way: a tab at chrome://settings/content/siteDetails?site=,
  // which the throttle turns into the window.
  chrome::ShowSiteSettings(browser(), site);
  ASSERT_TRUE(base::test::RunUntil([&] { return !!window(); }));
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  const std::string camera =
      "document.querySelector('select[data-type=\"media-stream-camera\"]')";
  // PermissionValue::kBlock.
  EXPECT_EQ(WaitFor(camera + " && " + camera + ".value"), "2");

  ASSERT_TRUE(content::ExecJs(window()->web_contents(),
                              camera + ".value = '1';" + camera +
                                  ".dispatchEvent(new Event('change'));"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return map->GetContentSetting(site, site,
                                  ContentSettingsType::MEDIASTREAM_CAMERA) ==
           CONTENT_SETTING_ALLOW;
  }));

  ASSERT_TRUE(
      content::ExecJs(window()->web_contents(),
                      "[...document.querySelectorAll('.actions button')].find("
                      "b => b.textContent.includes('Reset')).click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    content_settings::SettingInfo info;
    map->GetWebsiteSetting(site, site, ContentSettingsType::MEDIASTREAM_CAMERA,
                           &info);
    return info.primary_pattern.MatchesAllHosts();
  }));
}

// Privacy › Permissions: a default changes there.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, PrivacyPermissionDefaults) {
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile());
  chrome::ShowSettingsSubPage(browser(), "content");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  const std::string popups =
      "document.querySelector('select[data-type=\"popups\"]')";
  ASSERT_TRUE(WaitFor("!!" + popups).ExtractBool());
  const ContentSetting before =
      map->GetDefaultContentSetting(ContentSettingsType::POPUPS);
  const std::string other = before == CONTENT_SETTING_BLOCK ? "1" : "2";
  ASSERT_TRUE(content::ExecJs(window()->web_contents(),
                              popups + ".value = '" + other + "';" + popups +
                                  ".dispatchEvent(new Event('change'));"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return map->GetDefaultContentSetting(ContentSettingsType::POPUPS) != before;
  }));
}

// Privacy: the cookies menu and Safe Browsing write their prefs, and Clear
// browsing data clears and keeps its choices.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, PrivacyPrefsAndClearing) {
  PrefService* prefs = profile()->GetPrefs();
  chrome::ShowSettingsSubPage(browser(), "privacy");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  const std::string cookies =
      "document.querySelector('select[aria-label=\"Third-party cookies\"]')";
  ASSERT_TRUE(WaitFor("!!" + cookies).ExtractBool());
  ASSERT_TRUE(content::ExecJs(window()->web_contents(),
                              cookies + ".value = '1';" + cookies +
                                  ".dispatchEvent(new Event('change'));"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return prefs->GetInteger(::prefs::kCookieControlsMode) == 1; }));

  ASSERT_TRUE(content::ExecJs(window()->web_contents(), R"(
      const level = document.querySelector(
          'select[aria-label="Safe Browsing"]');
      level.value = '2';
      level.dispatchEvent(new Event('change'));)"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return prefs->GetBoolean(::prefs::kSafeBrowsingEnhanced); }));

  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[data-view=\"clear\"]').click();"));
  ASSERT_TRUE(WaitFor("!!document.querySelector('input.check')").ExtractBool());
  ASSERT_TRUE(content::ExecJs(window()->web_contents(), R"(
      for (const box of document.querySelectorAll('input.check')) {
        if (box.checked !== (box.dataset.key === 'cache')) {
          box.click();
        }
      }
      document.querySelector('.actions .accent').click();)"));
  EXPECT_EQ(WaitFor("document.querySelector('.actions .accent')"
                    ".textContent.trim() === 'Cleared'"),
            true);
  EXPECT_TRUE(prefs->GetBoolean(browsing_data::prefs::kDeleteCache));
  EXPECT_FALSE(prefs->GetBoolean(browsing_data::prefs::kDeleteBrowsingHistory));
}

// Passwords (phase 4): a saved password is listed, never with its secret,
// and removed there; so is an address. The switches write their prefs.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, PasswordsPane) {
  scoped_refptr<password_manager::PasswordStoreInterface> store =
      ProfilePasswordStoreFactory::GetForProfile(
          profile(), ServiceAccessType::EXPLICIT_ACCESS);
  password_manager::StoredCredential credential;
  credential.signon_realm = "https://example.com/";
  credential.url = GURL("https://example.com/login");
  credential.username_value = u"ada";
  credential.password_value =
      password_manager::PasswordString(u"correct horse battery staple");
  credential.in_store = password_manager::PasswordForm::Store::kProfileStore;
  base::RunLoop added;
  store->AddLogin(std::move(credential), added.QuitClosure());
  added.Run();

  autofill::PersonalDataManager* data =
      autofill::PersonalDataManagerFactory::GetForBrowserContext(profile());
  autofill::AutofillProfile address(autofill::AddressCountryCode("US"));
  address.SetRawInfo(autofill::NAME_FULL, u"Ada Lovelace");
  address.SetRawInfo(autofill::ADDRESS_HOME_LINE1, u"12 St James's Square");
  address.SetRawInfo(autofill::ADDRESS_HOME_CITY, u"London");
  data->address_data_manager().AddProfile(address);

  chrome::ShowSettingsSubPage(browser(), "passwords");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  EXPECT_EQ(WaitFor("document.querySelector('.password-row .row-detail')"
                    "?.textContent"),
            "ada");
  // The page has no password until the user asks (and authenticates).
  EXPECT_EQ(content::EvalJs(window()->web_contents(),
                            "document.body.textContent.includes("
                            "'correct horse')"),
            false);
  EXPECT_EQ(WaitFor("[...document.querySelectorAll('.row-label')].some("
                    "l => l.textContent === 'Ada Lovelace')"),
            true);

  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[aria-label=\"Remove example.com\"]')"
      ".click();"));
  EXPECT_EQ(WaitFor("!document.querySelector('.password-row')"), true);

  ASSERT_TRUE(content::ExecJs(window()->web_contents(), R"(
      [...document.querySelectorAll('.control-row')].find(
          r => r.textContent.includes('Ada Lovelace'))
          .querySelector('button').click();)"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return data->address_data_manager().GetProfiles().empty(); }));

  PrefService* prefs = profile()->GetPrefs();
  const bool offer =
      prefs->GetBoolean(password_manager::prefs::kCredentialsEnableService);
  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('input[data-pref=\"credentials_enable_service"
      "\"]').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return prefs->GetBoolean(
               password_manager::prefs::kCredentialsEnableService) != offer;
  }));
}

// Advanced (phase 5): memory saver writes Local State's pref, a channel that
// isn't one is refused, and Reset puts upstream's settings back.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, AdvancedPane) {
  PrefService* local_state = g_browser_process->local_state();
  local_state->SetInteger(
      performance_manager::user_tuning::prefs::kMemorySaverModeState, 0);
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile());
  const ContentSetting popups =
      map->GetDefaultContentSetting(ContentSettingsType::POPUPS);
  map->SetDefaultContentSetting(ContentSettingsType::POPUPS,
                                popups == CONTENT_SETTING_ALLOW
                                    ? CONTENT_SETTING_BLOCK
                                    : CONTENT_SETTING_ALLOW);

  chrome::ShowSettingsSubPage(browser(), "help");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  EXPECT_EQ(WaitFor("document.querySelector('compass-settings-advanced-pane "
                    "[data-row=\"updates\"] .row-label')?.textContent"
                    ".startsWith('Version')"),
            true);

  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[data-row=\"memorySaver\"] input').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return local_state->GetInteger(performance_manager::user_tuning::prefs::
                                       kMemorySaverModeState) == 2;
  }));

  // Not a channel: refused.
  EXPECT_EQ(content::EvalJs(window()->web_contents(), R"(
      (async () => {
        const {SettingsWindowBrowserProxy} =
            await import('/settings_window/browser_proxy.js');
        const {ok} = await SettingsWindowBrowserProxy.getInstance()
            .handler.setPref('compass.update_channel', '"nightly"');
        return ok;
      })())"),
            false);

  ASSERT_TRUE(content::ExecJs(window()->web_contents(), R"(
      document.querySelector('[data-row="reset"] .pill').click();)"));
  ASSERT_TRUE(
      WaitFor("!document.querySelector('.confirm-row').hidden").ExtractBool());
  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('.confirm-row .danger').click();"));
  EXPECT_EQ(WaitFor("document.querySelector('[data-row=\"reset\"] "
                    ".row-detail').textContent.trim() === "
                    "'Settings were reset.'"),
            true);
  EXPECT_EQ(map->GetDefaultContentSetting(ContentSettingsType::POPUPS),
            CONTENT_SETTING_BLOCK);
}

// General › Manage search engines (phase 5b): a site search is refused with
// a shortcut that has a space, added once it hasn't, then removed.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, SearchEngineManager) {
  TemplateURLService* service =
      TemplateURLServiceFactory::GetForProfile(profile());
  search_test_utils::WaitForTemplateURLServiceToLoad(service);
  chrome::ShowSettingsSubPage(browser(), "searchEngines");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  ASSERT_TRUE(
      WaitFor("!!document.querySelector('[data-engine]')").ExtractBool());

  const std::string fill = R"(
      (keyword) => {
        document.querySelector('.group-header .pill').click();
        return new Promise(resolve => setTimeout(() => {
          for (const [field, value] of [
                   ['name', 'Docs'], ['keyword', keyword],
                   ['url', 'https://docs.example.com/search?q=%s']]) {
            const input = document.querySelector(`[data-field="${field}"]`);
            input.value = value;
            input.dispatchEvent(new Event('input'));
          }
          setTimeout(() => {
            document.querySelector('.actions .accent').click();
            resolve(true);
          }, 50);
        }, 50));
      })";
  ASSERT_TRUE(
      content::ExecJs(window()->web_contents(), "(" + fill + ")('my docs');"));
  EXPECT_EQ(WaitFor("document.querySelector('.actions .error')?.textContent"
                    ".includes('shortcut')"),
            true);
  EXPECT_FALSE(service->GetTemplateURLForKeyword(u"my docs"));

  ASSERT_TRUE(content::ExecJs(window()->web_contents(), R"(
      const keyword = document.querySelector('[data-field="keyword"]');
      keyword.value = 'docs';
      keyword.dispatchEvent(new Event('input'));
      setTimeout(() => document.querySelector('.actions .accent').click(), 50);)"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return !!service->GetTemplateURLForKeyword(u"docs"); }));
  const std::string id = base::NumberToString(
      service->GetTemplateURLForKeyword(u"docs")->id().value());
  EXPECT_EQ(WaitFor("!!document.querySelector('[data-engine=\"" + id + "\"]')"),
            true);

  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "[...document.querySelectorAll('[data-engine=\"" + id +
          "\"] .pill')].find(b => b.textContent.trim() === 'Remove')"
          ".click();"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return !service->GetTemplateURLForKeyword(u"docs"); }));
}

// General › Languages (phase 5c): a language is added, moved first, and
// translation for it turned off; then removed.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, LanguagesEditor) {
  PrefService* prefs = profile()->GetPrefs();
  chrome::ShowSettingsSubPage(browser(), "languages");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  ASSERT_TRUE(WaitFor("!!document.querySelector('[aria-label=\"Add "
                      "Italian\"]')")
                  .ExtractBool());
  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[aria-label=\"Add Italian\"]').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return prefs->GetString(language::prefs::kAcceptLanguages).find("it") !=
           std::string::npos;
  }));

  // Up until it's first.
  for (int i = 0; i < 5; ++i) {
    if (prefs->GetString(language::prefs::kAcceptLanguages).starts_with("it")) {
      break;
    }
    ASSERT_TRUE(WaitFor("(() => { const b = document.querySelector("
                        "'[aria-label=\"Move Italian up\"]'); "
                        "return b && !b.disabled; })()")
                    .ExtractBool());
    const std::string before =
        prefs->GetString(language::prefs::kAcceptLanguages);
    ASSERT_TRUE(content::ExecJs(window()->web_contents(),
                                "document.querySelector('[aria-label=\"Move "
                                "Italian up\"]').click();"));
    ASSERT_TRUE(base::test::RunUntil([&] {
      return prefs->GetString(language::prefs::kAcceptLanguages) != before;
    }));
  }
  EXPECT_TRUE(
      prefs->GetString(language::prefs::kAcceptLanguages).starts_with("it"));

  // Added languages start blocked (the user reads them), as upstream's;
  // the switch flips that.
  const bool blocked =
      ChromeTranslateClient::CreateTranslatePrefs(prefs)->IsBlockedLanguage(
          "it");
  EXPECT_EQ(WaitFor("document.querySelector('[aria-label=\"Offer to "
                    "translate Italian\"]').checked === " +
                    std::string(blocked ? "false" : "true")),
            true);
  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[aria-label=\"Offer to translate "
      "Italian\"]').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return ChromeTranslateClient::CreateTranslatePrefs(prefs)
               ->IsBlockedLanguage("it") != blocked;
  }));

  ASSERT_TRUE(WaitFor("!!document.querySelector('[aria-label=\"Remove "
                      "Italian\"]')")
                  .ExtractBool());
  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[aria-label=\"Remove Italian\"]').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return prefs->GetString(language::prefs::kAcceptLanguages).find("it") ==
           std::string::npos;
  }));
}

}  // namespace
}  // namespace compass
