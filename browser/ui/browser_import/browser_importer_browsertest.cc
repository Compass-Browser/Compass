// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/browser_import/browser_importer.h"

#include <algorithm>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "compass/browser/browser_import/chromium_crypto.h"
#include "compass/browser/browser_import/crest_reader.h"
#include "compass/browser/browser_import/detect.h"
#include "compass/browser/browser_import/test_fixtures.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_service.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::browser_import {
namespace {

// 32 bytes: Windows' AES-256 key; elsewhere the Keychain password.
constexpr char kKey[] = "compass test key 0123456789abcde";

std::string EncryptPassword(const std::string& password) {
#if BUILDFLAG(IS_WIN)
  return EncryptWindowsValueForTesting(kKey, password);
#elif BUILDFLAG(IS_MAC)
  return arc::ChromiumMacCipher(kKey).Encrypt(password);
#else
  return arc::ChromiumMacCipher("peanuts", 1).Encrypt(password);
#endif
}

class BrowserImporterBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(dir_.CreateUniqueTempDir());
    user_data_ = dir_.GetPath().AppendASCII("Chrome");
    std::string os_crypt_key;
#if BUILDFLAG(IS_WIN)
    // Local State's key, wrapped with DPAPI as Chrome wraps it.
    os_crypt_key = WrapWindowsKeyForTesting(kKey);
#elif BUILDFLAG(IS_MAC)
    SetChromiumKeyForTesting(kKey);
#endif
    ASSERT_TRUE(test::WriteChromiumUserData(
        user_data_, "Person 1",
        {{"https://open.test/", u"Open"},
         {"https://trip.test/flights", u"Flights", false, u"Trip"},
         {"https://trip.test/hotels", u"Hotels", false, u"Trip"}},
        base::BindRepeating(&EncryptPassword), os_crypt_key));
    SetRootsForTesting(
        std::vector<BrowserRoot>{{Browser::kChrome, user_data_}});
  }

  void TearDownOnMainThread() override {
    SetRootsForTesting(std::nullopt);
    SetChromiumKeyForTesting(std::nullopt);
    InProcessBrowserTest::TearDownOnMainThread();
  }

  SpaceList& spaces() {
    return SpaceService::Get(browser()->GetProfile())->space_list();
  }

  base::ScopedTempDir dir_;
  base::FilePath user_data_;
};

IN_PROC_BROWSER_TEST_F(BrowserImporterBrowserTest, ListsTheChromeProfile) {
  base::test::TestFuture<std::vector<ProfileOverview>> future;
  GetBrowserImportOverview(browser()->GetProfile(), future.GetCallback());
  std::vector<ProfileOverview> profiles = future.Take();
  ASSERT_EQ(profiles.size(), 1u);
  EXPECT_EQ(profiles[0].source.id, "chrome:Default");
  EXPECT_EQ(profiles[0].source.name, u"Person 1");
  ASSERT_EQ(profiles[0].spaces.size(), 1u);
  EXPECT_EQ(profiles[0].spaces[0], u"Chrome");
  EXPECT_EQ(profiles[0].bookmarks, 3);
  EXPECT_EQ(profiles[0].tabs, 3);
  EXPECT_FALSE(profiles[0].running);
  EXPECT_FALSE(profiles[0].imported);
  EXPECT_TRUE(profiles[0].history);
  EXPECT_TRUE(profiles[0].passwords);
}

// A Chrome profile becomes a space: bookmarks in Pinned (the bar first),
// open tabs in Today (a titled group as a folder); its history and
// passwords come along.
IN_PROC_BROWSER_TEST_F(BrowserImporterBrowserTest, ImportsAChromeProfile) {
  const size_t spaces_before = spaces().spaces().size();
  base::test::TestFuture<BrowserImportResult> future;
  ImportFromBrowsers(browser()->GetProfile(), {{"chrome:Default"}},
                     future.GetCallback());
  BrowserImportResult result = future.Take();
  ASSERT_EQ(result.summaries.size(), 1u);
  const BrowserImportSummary& summary = result.summaries[0];
  EXPECT_EQ(summary.name, u"Chrome");
  ASSERT_EQ(summary.spaces.size(), 1u);
  EXPECT_EQ(summary.pinned, 3);
  EXPECT_EQ(summary.today, 3);
  EXPECT_EQ(summary.history, 2);
  EXPECT_EQ(summary.passwords, 1);
  EXPECT_EQ(summary.passwords_skipped, 0);
  EXPECT_TRUE(summary.problems.empty());

  ASSERT_EQ(spaces().spaces().size(), spaces_before + 1);
  SpaceModel* space = spaces().spaces().back().get();
  EXPECT_EQ(space->id(), result.first_space);
  EXPECT_EQ(space->name(), u"Chrome");
  EXPECT_FALSE(space->color().is_grey());
  const auto& pinned = space->pinned_root()->children();
  ASSERT_EQ(pinned.size(), 3u);
  EXPECT_EQ(pinned[0]->url(), GURL("https://bar.test/"));
  EXPECT_EQ(pinned[1]->type(), SidebarNode::Type::kFolder);
  EXPECT_EQ(pinned[1]->title(), u"Docs");
  EXPECT_EQ(pinned[2]->title(), u"Other bookmarks");
  const auto& today = space->today_root()->children();
  ASSERT_EQ(today.size(), 2u);
  EXPECT_EQ(today[0]->url(), GURL("https://open.test/"));
  EXPECT_EQ(today[1]->type(), SidebarNode::Type::kFolder);
  EXPECT_EQ(today[1]->title(), u"Trip");
  EXPECT_EQ(today[1]->children().size(), 2u);

  ui_test_utils::HistoryEnumerator history(browser()->GetProfile());
  EXPECT_TRUE(std::ranges::contains(history.urls(), GURL("https://history.test/a")));
  EXPECT_TRUE(std::ranges::contains(history.urls(), GURL("https://history.test/b")));

  // Listed as imported, and importing again adds no second space.
  base::test::TestFuture<std::vector<ProfileOverview>> overview;
  GetBrowserImportOverview(browser()->GetProfile(), overview.GetCallback());
  EXPECT_TRUE(overview.Take()[0].imported);
  base::test::TestFuture<BrowserImportResult> again;
  ImportFromBrowsers(browser()->GetProfile(),
                     {{"chrome:Default", true, true, false, false}},
                     again.GetCallback());
  EXPECT_TRUE(again.Take().summaries[0].spaces.empty());
  EXPECT_EQ(spaces().spaces().size(), spaces_before + 1);
}

IN_PROC_BROWSER_TEST_F(BrowserImporterBrowserTest, OnlyWhatIsChosen) {
  base::test::TestFuture<BrowserImportResult> future;
  ImportFromBrowsers(browser()->GetProfile(),
                     {{"chrome:Default", /*bookmarks=*/false, /*tabs=*/true,
                       /*history=*/false, /*passwords=*/false}},
                     future.GetCallback());
  BrowserImportResult result = future.Take();
  ASSERT_EQ(result.summaries.size(), 1u);
  EXPECT_EQ(result.summaries[0].pinned, 0);
  EXPECT_EQ(result.summaries[0].today, 3);
  EXPECT_EQ(result.summaries[0].history, 0);
  EXPECT_EQ(result.summaries[0].passwords, 0);
  SpaceModel* space = spaces().spaces().back().get();
  EXPECT_TRUE(space->pinned_root()->children().empty());
}

// A Chrome profile becomes a Compass profile of its own: copied, named
// after it, its space in place of the new profile's empty one, its source
// recorded; then listed as imported that way, and not copied again.
IN_PROC_BROWSER_TEST_F(BrowserImporterBrowserTest, ImportsAsAProfile) {
  {
    // The fixture's History and Login Data have only the columns the
    // readers use, and no meta table: loaded in a real profile, upstream
    // would take them for its current schema. Real ones have it; copying and
    // re-encrypting them is ProfileCopyTest's.
    base::ScopedAllowBlockingForTesting allow_blocking;
    const base::FilePath profile = user_data_.AppendASCII("Default");
    ASSERT_TRUE(base::DeleteFile(profile.AppendASCII("History")));
    ASSERT_TRUE(base::DeleteFile(profile.AppendASCII("Login Data")));
  }
  ProfileManager* manager = g_browser_process->profile_manager();
  const size_t profiles_before = manager->GetNumberOfProfiles();
  const size_t spaces_before = spaces().spaces().size();
  BrowserImportChoice choice;
  choice.id = "chrome:Default";
  choice.new_profile = true;
  base::test::TestFuture<BrowserImportResult> future;
  ImportFromBrowsers(browser()->GetProfile(), {choice}, future.GetCallback());
  BrowserImportResult result = future.Take();
  ASSERT_EQ(result.summaries.size(), 1u);
  const BrowserImportSummary& summary = result.summaries[0];
  EXPECT_TRUE(summary.problems.empty());
  EXPECT_EQ(summary.profile, u"Chrome");
  ASSERT_EQ(summary.spaces.size(), 1u);
  EXPECT_EQ(summary.pinned, 3);
  EXPECT_EQ(summary.today, 3);
  EXPECT_EQ(summary.history, -1);
  EXPECT_EQ(summary.passwords, -1);

  EXPECT_EQ(manager->GetNumberOfProfiles(), profiles_before + 1);
  ASSERT_TRUE(result.first_space_profile);
  Profile* imported = result.first_space_profile.get();
  EXPECT_NE(imported, browser()->GetProfile());
  EXPECT_EQ(imported->GetPrefs()->GetString(prefs::kBrowserImportProfile),
            "chrome:Default");
  SpaceList& list = SpaceService::Get(imported)->space_list();
  ASSERT_EQ(list.spaces().size(), 1u);
  EXPECT_EQ(list.spaces()[0]->id(), result.first_space);
  EXPECT_EQ(list.spaces()[0]->name(), u"Chrome");
  EXPECT_EQ(list.spaces()[0]->pinned_root()->children().size(), 3u);
  // This profile is left alone.
  EXPECT_EQ(spaces().spaces().size(), spaces_before);

  base::test::TestFuture<std::vector<ProfileOverview>> overview;
  GetBrowserImportOverview(browser()->GetProfile(), overview.GetCallback());
  const std::vector<ProfileOverview> profiles = overview.Take();
  ASSERT_EQ(profiles.size(), 1u);
  EXPECT_TRUE(profiles[0].as_profile);
  EXPECT_EQ(profiles[0].imported_as_profile, u"Chrome");
  EXPECT_FALSE(profiles[0].imported);

  base::test::TestFuture<BrowserImportResult> again;
  ImportFromBrowsers(browser()->GetProfile(), {choice}, again.GetCallback());
  const BrowserImportResult again_result = again.Take();
  ASSERT_EQ(again_result.summaries.size(), 1u);
  EXPECT_EQ(again_result.summaries[0].problems,
            std::vector<std::string>{"It was imported as a profile before."});
  EXPECT_EQ(manager->GetNumberOfProfiles(), profiles_before + 1);
}

// A Crest profile: its Chromium profile copied, its spaces from Crest's own
// session and its history (not in the folder) written into it.
IN_PROC_BROWSER_TEST_F(BrowserImporterBrowserTest, ImportsACrestProfile) {
  const base::FilePath crest = dir_.GetPath().AppendASCII("Crest");
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    // The fixture's Chrome profile as Crest's Chromium profile P1.
    const base::FilePath chromium = crest.AppendASCII("Chromium");
    ASSERT_TRUE(base::CreateDirectory(chromium));
    const base::FilePath folder = chromium.AppendASCII("Crest-P1");
    ASSERT_TRUE(base::CopyDirectory(user_data_.AppendASCII("Default"), folder,
                                    /*recursive=*/true));
    ASSERT_TRUE(base::DeleteFile(folder.AppendASCII("History")));
    ASSERT_TRUE(base::DeleteFile(folder.AppendASCII("Login Data")));
    ASSERT_TRUE(test::WriteCrestSession(
        CrestSessionPath(crest),
        R"({"spaces": [{"id": {"rawValue": "S1"}, "profile": {"id": "P1"},
            "name": "Work", "accent": "teal", "folders": [],
            "tabs": [{"id": {"rawValue": "T1"}, "title": "Docs",
                      "url": "https://docs.test/", "placement": "saved"},
                     {"id": {"rawValue": "T2"}, "title": "News",
                      "url": "https://news.test/", "placement": "current"}]}]})",
        {{"S1", R"([{"url": "https://docs.test/", "title": "Docs",
                     "visitCount": 3, "lastVisitedAt": 785000000}])"}}));
  }
  SetRootsForTesting(std::vector<BrowserRoot>{{Browser::kCrest, crest}});

  base::test::TestFuture<std::vector<ProfileOverview>> overview;
  GetBrowserImportOverview(browser()->GetProfile(), overview.GetCallback());
  const std::vector<ProfileOverview> profiles = overview.Take();
  ASSERT_EQ(profiles.size(), 1u);
  EXPECT_EQ(profiles[0].source.id, "crest:P1");
  EXPECT_TRUE(profiles[0].as_profile);
  EXPECT_FALSE(profiles[0].passwords);
  EXPECT_NE(profiles[0].as_profile_brings.find("Crest's passwords"),
            std::string::npos);

  ProfileManager* manager = g_browser_process->profile_manager();
  const size_t profiles_before = manager->GetNumberOfProfiles();
  BrowserImportChoice choice;
  choice.id = "crest:P1";
  choice.new_profile = true;
  base::test::TestFuture<BrowserImportResult> future;
  ImportFromBrowsers(browser()->GetProfile(), {choice}, future.GetCallback());
  BrowserImportResult result = future.Take();
  ASSERT_EQ(result.summaries.size(), 1u);
  const BrowserImportSummary& summary = result.summaries[0];
  EXPECT_TRUE(summary.problems.empty());
  EXPECT_EQ(summary.profile, u"Crest");
  EXPECT_EQ(summary.spaces, std::vector<std::u16string>{u"Work"});
  EXPECT_EQ(summary.pinned, 1);
  EXPECT_EQ(summary.today, 1);
  EXPECT_EQ(summary.history, 1);
  EXPECT_EQ(summary.passwords, 0);
  EXPECT_EQ(summary.brought, "cookies, history and extensions");

  EXPECT_EQ(manager->GetNumberOfProfiles(), profiles_before + 1);
  ASSERT_TRUE(result.first_space_profile);
  Profile* imported = result.first_space_profile.get();
  EXPECT_EQ(imported->GetPrefs()->GetString(prefs::kBrowserImportProfile),
            "crest:P1");
  SpaceList& list = SpaceService::Get(imported)->space_list();
  ASSERT_EQ(list.spaces().size(), 1u);
  EXPECT_EQ(list.spaces()[0]->name(), u"Work");
}

// A Yab profile that never opened a site in Chromium: no folder to copy, a
// new profile with its spaces and history.
IN_PROC_BROWSER_TEST_F(BrowserImporterBrowserTest, ImportsAYabProfile) {
  const base::FilePath yab = dir_.GetPath().AppendASCII("Yab");
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::CreateDirectory(yab));
    ASSERT_TRUE(base::WriteFile(yab.AppendASCII("spaces.json"), R"([
      {"id": "00000000-0000-0000-0000-000000000001", "name": "Personal"},
      {"id": "AAAA", "name": "Work", "ownSignIns": true}])"));
    ASSERT_TRUE(base::WriteFile(yab.AppendASCII("session-AAAA.json"), R"({
      "tabs": [{"url": "https://work.test/", "title": "Work", "kept": true,
                "id": "t1"},
               {"url": "https://mail.test/", "title": "Mail", "id": "t2"}]})"));
    ASSERT_TRUE(base::WriteFile(yab.AppendASCII("history-AAAA.json"), R"([
      {"url": "https://work.test/", "title": "Work", "count": 2,
       "last": 785000000},
      {"url": "https://mail.test/", "title": "Mail", "count": 1,
       "last": 785000000}])"));
  }
  SetRootsForTesting(std::vector<BrowserRoot>{{Browser::kYab, yab}});

  base::test::TestFuture<std::vector<ProfileOverview>> overview;
  GetBrowserImportOverview(browser()->GetProfile(), overview.GetCallback());
  const std::vector<ProfileOverview> profiles = overview.Take();
  ASSERT_EQ(profiles.size(), 2u);
  EXPECT_EQ(profiles[1].source.id, "yab:AAAA");
  EXPECT_TRUE(profiles[1].as_profile);

  ProfileManager* manager = g_browser_process->profile_manager();
  const size_t profiles_before = manager->GetNumberOfProfiles();
  BrowserImportChoice choice;
  choice.id = "yab:AAAA";
  choice.new_profile = true;
  base::test::TestFuture<BrowserImportResult> future;
  ImportFromBrowsers(browser()->GetProfile(), {choice}, future.GetCallback());
  BrowserImportResult result = future.Take();
  ASSERT_EQ(result.summaries.size(), 1u);
  const BrowserImportSummary& summary = result.summaries[0];
  EXPECT_TRUE(summary.problems.empty());
  EXPECT_EQ(summary.profile, u"Yab – Work");
  EXPECT_EQ(summary.pinned, 1);
  EXPECT_EQ(summary.today, 1);
  EXPECT_EQ(summary.history, 2);

  EXPECT_EQ(manager->GetNumberOfProfiles(), profiles_before + 1);
  ASSERT_TRUE(result.first_space_profile);
  Profile* imported = result.first_space_profile.get();
  EXPECT_EQ(imported->GetPrefs()->GetString(prefs::kBrowserImportProfile),
            "yab:AAAA");
  SpaceList& list = SpaceService::Get(imported)->space_list();
  ASSERT_EQ(list.spaces().size(), 1u);
  EXPECT_EQ(list.spaces()[0]->name(), u"Work");

  // Imported once.
  base::test::TestFuture<BrowserImportResult> again;
  ImportFromBrowsers(browser()->GetProfile(), {choice}, again.GetCallback());
  EXPECT_EQ(again.Take().summaries[0].problems,
            std::vector<std::string>{"It was imported as a profile before."});
}

// A profile a newer Chromium last opened is flagged: a copy may lose data.
IN_PROC_BROWSER_TEST_F(BrowserImporterBrowserTest, FlagsANewerChromium) {
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(
        base::WriteFile(user_data_.AppendASCII("Last Version"), "999.0.0.0"));
  }
  base::test::TestFuture<std::vector<ProfileOverview>> overview;
  GetBrowserImportOverview(browser()->GetProfile(), overview.GetCallback());
  const std::vector<ProfileOverview> profiles = overview.Take();
  ASSERT_EQ(profiles.size(), 1u);
  EXPECT_EQ(profiles[0].newer_version, 999);
}

// The page lists the profile; its row opens its choices, which import it
// and sum up (docs/design/browser-import.md, "The page").
IN_PROC_BROWSER_TEST_F(BrowserImporterBrowserTest, ImportPage) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://compass-import/")));
  content::WebContents* page =
      browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(page));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page,
                           "document.querySelectorAll('#browsers .browser')"
                           ".length")
               .ExtractInt() == 1;
  }));
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelector('#browsers .row-label')"
                            ".textContent"),
            "Chrome · Person 1");
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelector('#browsers .row-detail')"
                            ".textContent"),
            "3 bookmarks · 3 open tabs");

  // Its row opens its choices.
  ASSERT_TRUE(content::ExecJs(
      page, "document.querySelector('#browsers button.browser').click();"));
  EXPECT_EQ(false, content::EvalJs(
                       page, "document.getElementById('browserView').hidden"));
  EXPECT_EQ(true, content::EvalJs(
                      page, "document.getElementById('listView').hidden"));
  EXPECT_EQ(content::EvalJs(page,
                            "document.getElementById('browserTitle')"
                            ".textContent"),
            "Chrome · Person 1");
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelectorAll('#options "
                            ".option-check').length"),
            4);
  // Chrome can also become a profile; a space here is the default.
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelector('.mode-radio:checked')"
                            ".value"),
            "space");
  // No passwords this time.
  ASSERT_TRUE(content::ExecJs(
      page,
      "document.querySelector('.option-check[data-kind=passwords]').click();"
      "document.getElementById('importBrowsers').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page,
                           "!document.getElementById('browsersDone').hidden")
        .ExtractBool();
  }));
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelector('#browsersSummary "
                            ".row-detail').textContent"),
            "Space Chrome · 3 tabs in Pinned · 3 tabs in Today "
            "· 2 history pages");
  EXPECT_EQ(false, content::EvalJs(
                       page, "document.getElementById('openBrowsers').hidden"));

  // "Import Another Browser" goes back to the list.
  ASSERT_TRUE(content::ExecJs(
      page, "document.getElementById('browsersAgain').click();"));
  EXPECT_EQ(false, content::EvalJs(
                       page, "document.getElementById('listView').hidden"));
}

// A browser's row shows its app's own icon, or the first letter of its name
// where its app isn't on this computer (app_icon.h).
IN_PROC_BROWSER_TEST_F(BrowserImporterBrowserTest, ImportPageShowsAnIcon) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://compass-import/")));
  content::WebContents* page =
      browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(page));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page,
                           "document.querySelectorAll('#browsers .browser')"
                           ".length")
               .ExtractInt() == 1;
  }));
  EXPECT_EQ(true, content::EvalJs(
                      page,
                      "(() => {"
                      "  const row = document.querySelector("
                      "      '#browsers .browser');"
                      "  const icon = row.querySelector('img.app-icon');"
                      "  if (icon) {"
                      "    return icon.src.startsWith('data:image/png;');"
                      "  }"
                      "  return row.querySelector('.tile.letter')"
                      "      ?.textContent === 'C';"
                      "})()"));
}

}  // namespace
}  // namespace compass::browser_import
