// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/views/settings_window/settings_window.h"

#include <algorithm>
#include <string>
#include <string_view>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/test/run_until.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/autofill/personal_data_manager_factory.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/password_manager/factories/profile_password_store_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/profiles/profile_test_util.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/search_test_utils.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "components/autofill/core/browser/data_manager/addresses/address_data_manager.h"
#include "components/autofill/core/browser/data_manager/payments/payments_data_manager.h"
#include "components/autofill/core/browser/data_manager/personal_data_manager.h"
#include "components/autofill/core/browser/data_model/addresses/autofill_profile.h"
#include "components/autofill/core/browser/data_model/payments/credit_card.h"
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
#include "ui/shell_dialogs/fake_select_file_dialog.h"
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
  // Upstream's sections open their panes; any other opens the window.
  EXPECT_EQ(SettingsWindowPathFor("searchEngines"), "general?view=engines");
  EXPECT_EQ(SettingsWindowPathFor("languages"), "general?view=languages");
  EXPECT_EQ(SettingsWindowPathFor("ai"), "");
  EXPECT_EQ(SettingsWindowPathFor("safetyCheck"), "privacy?view=safety");
  EXPECT_EQ(SettingsWindowPathFor("people"), "profiles");
  EXPECT_EQ(SettingsWindowPathFor("syncSetup/advanced"), "sync");
  EXPECT_EQ(SettingsWindowPathFor("noSuchSection"), "");
  EXPECT_EQ(SettingsWindowPathFor("payments"), "passwords?focus=saveCards");
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
            11);
  // They fit in the toolbar.
  EXPECT_EQ(content::EvalJs(window()->web_contents(),
                            "(() => { const tabs = document.querySelector("
                            "'#tabs'); return tabs.scrollWidth <= "
                            "tabs.clientWidth; })()"),
            true);

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

// Every chrome://settings section opens the window: upstream's settings
// page is never shown (phase 5f).
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, EverySectionOpensTheWindow) {
  const int tabs = tab_count();
  chrome::ShowSettingsSubPage(browser(), "people");
  ASSERT_TRUE(window());
  EXPECT_EQ(tab_count(), tabs);
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  EXPECT_EQ(WaitFor("document.querySelector('.tab[aria-selected=true]')"
                    "?.dataset.pane"),
            "profiles");
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
      "document.querySelector('[data-row=\"profileName\"] input')";
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
}

// Profiles lists every profile: another one's name is saved too.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, RenamesAnotherProfile) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& other = profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  chrome::ShowSettingsSubPage(browser(), "profiles");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  const std::string field = content::JsReplace(
      "document.querySelector('input[data-profile-id=\"' + $1 + '\"]')",
      other.GetBaseName().AsUTF8Unsafe());
  ASSERT_TRUE(WaitFor(field).is_ok());
  ASSERT_TRUE(content::ExecJs(window()->web_contents(),
                              field + ".value = 'Side project';" + field +
                                  ".dispatchEvent(new Event('change'));"));
  ProfileAttributesEntry* entry =
      profile_manager->GetProfileAttributesStorage()
          .GetProfileAttributesWithPath(other.GetPath());
  ASSERT_TRUE(entry);
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return entry->GetLocalProfileName() == u"Side project"; }));
  // The window's own profile keeps its name.
  EXPECT_NE(profile_manager->GetProfileAttributesStorage()
                .GetProfileAttributesWithPath(profile()->GetPath())
                ->GetLocalProfileName(),
            u"Side project");
}

// Profiles: "New profile" makes one, which joins the switcher; the list shows
// it.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, CreatesProfile) {
  ProfileAttributesStorage& storage =
      g_browser_process->profile_manager()->GetProfileAttributesStorage();
  const size_t before = storage.GetNumberOfProfiles();
  chrome::ShowSettingsSubPage(browser(), "profiles");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  ASSERT_TRUE(
      WaitFor("document.querySelector('[data-row=\"addProfile\"]')").is_ok());
  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[data-row=\"addProfile\"]').click()"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return storage.GetNumberOfProfiles() == before + 1; }));
  EXPECT_EQ(WaitFor("document.querySelectorAll('.profile-row').length === " +
                    base::NumberToString(before + 1))
                .ExtractBool(),
            true);
}

// Profiles: another profile is deleted once confirmed.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, DeletesProfile) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& other = profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  const base::FilePath path = other.GetPath();
  chrome::ShowSettingsSubPage(browser(), "profiles");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  const std::string id = other.GetBaseName().AsUTF8Unsafe();
  const std::string ask = content::JsReplace(
      "document.querySelector('.profile-row button[data-profile-id=\"' + $1 + "
      "'\"]')",
      id);
  ASSERT_TRUE(WaitFor(ask).is_ok());
  ASSERT_TRUE(content::ExecJs(window()->web_contents(), ask + ".click()"));
  const std::string confirm = content::JsReplace(
      "document.querySelector('.confirm-row:not([hidden]) "
      "button[data-profile-id=\"' + $1 + '\"]')",
      id);
  ASSERT_TRUE(WaitFor(confirm).is_ok());
  ASSERT_TRUE(content::ExecJs(window()->web_contents(), confirm + ".click()"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    ProfileAttributesEntry* entry =
        profile_manager->GetProfileAttributesStorage()
            .GetProfileAttributesWithPath(path);
    return !entry || entry->IsOmitted();
  }));
}

// Spaces lists every profile's spaces, and edits, adds, moves and deletes
// another profile's.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, SpacesPaneEditsAnyProfile) {
  ProfileManager* profile_manager = g_browser_process->profile_manager();
  Profile& other = profiles::testing::CreateProfileSync(
      profile_manager, profile_manager->GenerateNextProfileDirectoryPath());
  SpaceService* service = SpaceService::Get(&other);
  {
    base::RunLoop loaded;
    service->RunWhenLoaded(loaded.QuitClosure());
    loaded.Run();
  }
  SpaceDirectory::Get()->List(&other);
  SpaceList& list = service->space_list();
  ASSERT_EQ(list.spaces().size(), 1u);
  SpaceModel& home = *list.spaces()[0];
  const std::string home_id = home.id().AsLowercaseString();

  chrome::ShowSettingsSubPage(browser(), "spaces");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  content::WebContents* page = window()->web_contents();

  // Rename it.
  const std::string name = content::JsReplace(
      "document.querySelector('input.space-name[data-space-id=\"' + $1 + "
      "'\"]')",
      home_id);
  ASSERT_TRUE(WaitFor(name).is_ok());
  ASSERT_TRUE(
      content::ExecJs(page, name + ".value = 'Errands';" + name +
                                ".dispatchEvent(new Event('change'));"));
  ASSERT_TRUE(base::test::RunUntil([&] { return home.name() == u"Errands"; }));

  // Add one to that profile.
  const std::string add = content::JsReplace(
      "document.querySelector('.group-header button[data-profile-id=\"' + $1 "
      "+ '\"]')",
      other.GetBaseName().AsUTF8Unsafe());
  ASSERT_TRUE(content::ExecJs(page, add + ".click()"));
  ASSERT_TRUE(base::test::RunUntil([&] { return list.spaces().size() == 2u; }));
  SpaceModel& added = *list.spaces()[1];
  const std::string added_id = added.id().AsLowercaseString();

  // Move the new one above Errands.
  const std::string up = content::JsReplace(
      "document.querySelector('button.move[data-step=\"up\"][data-space-id="
      "\"' + $1 + '\"]')",
      added_id);
  ASSERT_TRUE(WaitFor(up + " && !" + up + ".disabled").is_ok());
  ASSERT_TRUE(content::ExecJs(page, up + ".click()"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    std::vector<SpaceDirectory::Entry> spaces =
        SpaceDirectory::Get()->GetSpaces();
    auto at = [&](const SpaceModel* space) {
      return std::ranges::find(spaces, space, &SpaceDirectory::Entry::space);
    };
    return at(&added) < at(&home);
  }));

  // Delete Errands, once confirmed.
  const std::string edit = content::JsReplace(
      "document.querySelector('.space-row button[aria-expanded]"
      "[data-space-id=\"' + $1 + '\"]')",
      home_id);
  ASSERT_TRUE(content::ExecJs(page, edit + ".click()"));
  const std::string ask = content::JsReplace(
      "document.querySelector('.space-details .space-detail-row "
      "button.danger[data-space-id=\"' + $1 + '\"]')",
      home_id);
  ASSERT_TRUE(WaitFor(ask).is_ok());
  ASSERT_TRUE(content::ExecJs(page, ask + ".click()"));
  const std::string confirm = content::JsReplace(
      "document.querySelector('.confirm-row:not([hidden]) "
      "button[data-space-id=\"' + $1 + '\"]')",
      home_id);
  ASSERT_TRUE(WaitFor(confirm).is_ok());
  ASSERT_TRUE(content::ExecJs(page, confirm + ".click()"));
  ASSERT_TRUE(base::test::RunUntil([&] { return list.spaces().size() == 1u; }));
  EXPECT_EQ(list.spaces()[0].get(), &added);
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
// Spaces pane → Export (docs/design/spaces.md, "Export"): the file the user
// picks gets every space's pinned tabs as a bookmarks file.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, SpacesPaneExports) {
  SpaceService* service = SpaceService::Get(browser()->GetProfile());
  {
    base::RunLoop loaded;
    service->RunWhenLoaded(loaded.QuitClosure());
    loaded.Run();
  }
  SpaceModel& space = service->space_list().active_space();
  space.SetName(u"Errands");
  space.AddTab(space.pinned_root(), 0, GURL("https://example.com/list"),
               u"Shopping list");

  ui::FakeSelectFileDialog::Factory* factory =
      ui::FakeSelectFileDialog::RegisterFactory();
  base::RunLoop opened;
  factory->SetOpenCallback(opened.QuitClosure());
  chrome::ShowSettingsSubPage(browser(), "spaces");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  const std::string button = "document.querySelector('#exportAll')";
  ASSERT_TRUE(WaitFor(button).is_ok());
  ASSERT_TRUE(content::ExecJs(window()->web_contents(), button + ".click()"));
  opened.Run();
  EXPECT_EQ(factory->GetLastDialog()->title(), u"Export Spaces");

  base::ScopedAllowBlockingForTesting allow_blocking;
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  const base::FilePath path = dir.GetPath().AppendASCII("spaces.html");
  ASSERT_TRUE(factory->GetLastDialog()->CallFileSelected(path, "html"));
  std::string html;
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return base::ReadFileToString(path, &html) && !html.empty(); }));
  EXPECT_TRUE(html.starts_with("<!DOCTYPE NETSCAPE-Bookmark-file-1>"));
  EXPECT_NE(html.find("<DT><H3>Errands</H3>"), std::string::npos);
  EXPECT_NE(html.find("<DT><A HREF=\"https://example.com/list\">Shopping "
                      "list</A>"),
            std::string::npos);
}

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
          .querySelector('button[data-guid]:last-of-type').click();)"));
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
// Whether |code| is one of the accept languages (a whole entry: "is" isn't
// in "en-US,it").
bool HasAcceptLanguage(PrefService* prefs, std::string_view code) {
  const std::string languages =
      prefs->GetString(language::prefs::kAcceptLanguages);
  return std::ranges::contains(
      base::SplitStringPiece(languages, ",", base::TRIM_WHITESPACE,
                             base::SPLIT_WANT_NONEMPTY),
      code);
}

// Icelandic: a language no test machine should have already (win1's Windows
// is in Italian, which the test first used). Buttons are found by language
// code and verb, as their names are in the UI's language (Italian on win1).
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, LanguagesEditor) {
  PrefService* prefs = profile()->GetPrefs();
  chrome::ShowSettingsSubPage(browser(), "languages");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  ASSERT_TRUE(WaitFor("!!document.querySelector('[data-code=\"is\"]"
                      "[aria-label^=\"Add \"]')")
                  .ExtractBool());
  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[data-code=\"is\"][aria-label^=\"Add \"]')"
      ".click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return HasAcceptLanguage(prefs, "is");
  }));

  // Up until it's first.
  for (int i = 0; i < 5; ++i) {
    if (prefs->GetString(language::prefs::kAcceptLanguages).starts_with("is")) {
      break;
    }
    ASSERT_TRUE(WaitFor("(() => { const b = document.querySelector("
                        "'[data-code=\"is\"][aria-label^=\"Move \"]"
                        "[aria-label$=\" up\"]'); "
                        "return b && !b.disabled; })()")
                    .ExtractBool());
    const std::string before =
        prefs->GetString(language::prefs::kAcceptLanguages);
    ASSERT_TRUE(content::ExecJs(window()->web_contents(),
                                "document.querySelector('[data-code=\"is\"]"
                                "[aria-label^=\"Move "
                                "\"][aria-label$=\" up\"]').click();"));
    ASSERT_TRUE(base::test::RunUntil([&] {
      return prefs->GetString(language::prefs::kAcceptLanguages) != before;
    }));
  }
  EXPECT_TRUE(
      prefs->GetString(language::prefs::kAcceptLanguages).starts_with("is"));

  // Added languages start blocked (the user reads them), as upstream's;
  // the switch flips that.
  const bool blocked =
      ChromeTranslateClient::CreateTranslatePrefs(prefs)->IsBlockedLanguage(
          "is");
  EXPECT_EQ(WaitFor("document.querySelector('[data-code=\"is\"]"
                    "[aria-label^=\"Offer "
                    "to translate \"]').checked === " +
                    std::string(blocked ? "false" : "true")),
            true);
  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[data-code=\"is\"][aria-label^=\"Offer to "
      "translate \"]').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return ChromeTranslateClient::CreateTranslatePrefs(prefs)
               ->IsBlockedLanguage("is") != blocked;
  }));

  ASSERT_TRUE(WaitFor("!!document.querySelector('[data-code=\"is\"]"
                      "[aria-label^=\"Remove \"]')")
                  .ExtractBool());
  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[data-code=\"is\"][aria-label^=\"Remove "
      "\"]').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !HasAcceptLanguage(prefs, "is");
  }));
}

// Passwords › the card and address editors (phase 5d): a card is refused
// with a number that isn't one, added with one; an address is added, then
// edited.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, CardAndAddressEditors) {
  autofill::PersonalDataManager* data =
      autofill::PersonalDataManagerFactory::GetForBrowserContext(profile());
  chrome::ShowSettingsSubPage(browser(), "passwords");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  content::WebContents* page = window()->web_contents();
  // Sets the fields (data-field) of the open editor, then clicks Save.
  const std::string fill_and_save = R"(
      (fields) => new Promise(resolve => setTimeout(() => {
        for (const [field, value] of Object.entries(fields)) {
          const input = document.querySelector(`[data-field="${field}"]`);
          input.value = value;
          input.dispatchEvent(new Event(
              input.tagName === 'SELECT' ? 'change' : 'input'));
        }
        setTimeout(() => {
          [...document.querySelectorAll('.actions .accent')]
              .find(b => b.offsetParent).click();
          resolve(true);
        }, 50);
      }, 50)))";
  const std::string click = R"(
      (label) => [...document.querySelectorAll('button.pill')]
                     .find(b => b.textContent.trim() === label).click())";

  ASSERT_TRUE(WaitFor("[...document.querySelectorAll('button.pill')].some("
                      "b => b.textContent.trim() === 'Add Card…')")
                  .ExtractBool());
  ASSERT_TRUE(content::ExecJs(page, "(" + click + ")('Add Card…');"));
  ASSERT_TRUE(content::ExecJs(
      page,
      "(" + fill_and_save +
          ")({nameOnCard: 'Ada Lovelace', number: '4111 1111 1111 1112'});"));
  EXPECT_EQ(WaitFor("document.querySelector('.actions .error:not([hidden])')"
                    "?.textContent.includes('number')"),
            true);
  ASSERT_TRUE(content::ExecJs(
      page, "(" + fill_and_save + ")({number: '4111 1111 1111 1111'});"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return data->payments_data_manager().GetCreditCards().size() == 1u;
  }));
  EXPECT_EQ(data->payments_data_manager().GetCreditCards()[0]->LastFourDigits(),
            u"1111");

  ASSERT_TRUE(content::ExecJs(page, "(" + click + ")('Add Address…');"));
  ASSERT_TRUE(content::ExecJs(
      page, "(" + fill_and_save +
                ")({countryCode: 'GB', fullName: 'Ada Lovelace', "
                "street: '12 St James\\'s Square', city: 'London'});"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return data->address_data_manager().GetProfiles().size() == 1u; }));
  const std::string guid =
      data->address_data_manager().GetProfiles()[0]->guid();
  EXPECT_EQ(data->address_data_manager().GetProfiles()[0]->GetRawInfo(
                autofill::ADDRESS_HOME_CITY),
            u"London");

  ASSERT_TRUE(
      WaitFor("!!document.querySelector('[data-guid=\"" + guid + "\"]')")
          .ExtractBool());
  ASSERT_TRUE(content::ExecJs(
      page, "document.querySelector('[data-guid=\"" + guid + "\"]').click();"));
  ASSERT_TRUE(WaitFor("document.querySelector('[data-field=\"city\"]')?.value "
                      "=== 'London'")
                  .ExtractBool());
  ASSERT_TRUE(
      content::ExecJs(page, "(" + fill_and_save + ")({city: 'Cambridge'});"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    const autofill::AutofillProfile* address =
        data->address_data_manager().GetProfileByGUID(guid);
    return address &&
           address->GetRawInfo(autofill::ADDRESS_HOME_CITY) == u"Cambridge";
  }));
}

// Privacy › Safety check (phase 5e): Safe Browsing off is flagged and
// turned on there; with no extensions, there are none to flag.
IN_PROC_BROWSER_TEST_F(SettingsWindowBrowserTest, SafetyCheck) {
  PrefService* prefs = profile()->GetPrefs();
  safe_browsing::SetSafeBrowsingState(
      prefs, safe_browsing::SafeBrowsingState::NO_SAFE_BROWSING);
  chrome::ShowSettingsSubPage(browser(), "safetyCheck");
  ASSERT_TRUE(window());
  ASSERT_TRUE(content::WaitForLoadStop(window()->web_contents()));
  EXPECT_EQ(WaitFor("document.querySelector('[data-check=\"extensions\"] "
                    ".row-detail')?.textContent.startsWith('No ')"),
            true);
  EXPECT_EQ(WaitFor("document.querySelector('[data-check=\"safeBrowsing\"] "
                    ".status')?.hasAttribute('ok') === false"),
            true);
  ASSERT_TRUE(content::ExecJs(
      window()->web_contents(),
      "document.querySelector('[data-check=\"safeBrowsing\"] button')"
      ".click();"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return prefs->GetBoolean(::prefs::kSafeBrowsingEnabled); }));
  EXPECT_EQ(WaitFor("document.querySelector('[data-check=\"safeBrowsing\"] "
                    ".status')?.hasAttribute('ok')"),
            true);
}

}  // namespace
}  // namespace compass
