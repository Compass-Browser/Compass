// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/browser_import/browser_importer.h"

#include <algorithm>

#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/test/run_until.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "compass/browser/browser_import/chromium_crypto.h"
#include "compass/browser/browser_import/detect.h"
#include "compass/browser/browser_import/test_fixtures.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_service.h"
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

// The page lists the profile, imports it and sums up.
IN_PROC_BROWSER_TEST_F(BrowserImporterBrowserTest, ImportPage) {
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://compass-import/")));
  content::WebContents* page =
      browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(page));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page,
                           "document.querySelectorAll('#browsers li.browser')"
                           ".length")
               .ExtractInt() == 1;
  }));
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelector('#browsers .name')"
                            ".textContent"),
            "Chrome – Person 1");
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelector('#browsers .details')"
                            ".textContent"),
            "3 bookmarks · 3 open tabs");
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelectorAll('#browsers "
                            ".option-check').length"),
            4);
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
                            ".details').textContent"),
            "Space Chrome · 3 tabs in Pinned · 3 tabs in Today "
            "· 2 history pages");
  EXPECT_EQ(false, content::EvalJs(
                       page, "document.getElementById('openBrowsers').hidden"));
}

}  // namespace
}  // namespace compass::browser_import
