// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/arc_import/arc_importer.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_writer.h"
#include "base/run_loop.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/prefs/chrome_pref_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "base/test/run_until.h"
#include "chrome/test/base/ui_test_utils.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::arc {
namespace {

constexpr char kArcPassword[] = "arc test password";

class ArcImporterBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(arc_dir_.CreateUniqueTempDir());
    ArcPaths paths;
    paths.user_data = arc_dir_.GetPath().AppendASCII("User Data");
    paths.cache = arc_dir_.GetPath().AppendASCII("Caches");
    paths.sidebar = arc_dir_.GetPath().AppendASCII("StorableSidebar.json");
    SetArcPathsForTesting(paths);
    SetArcKeychainPasswordForTesting(kArcPassword);
    SetArcRunningForTesting(false);
    arc_profile_ = paths.user_data.AppendASCII("Default");
  }

  void TearDownOnMainThread() override {
    SetArcPathsForTesting(std::nullopt);
    SetArcKeychainPasswordForTesting(std::nullopt);
    SetArcRunningForTesting(std::nullopt);
    InProcessBrowserTest::TearDownOnMainThread();
  }

  scoped_refptr<os_crypt_async::Encryptor> GetEncryptor() {
    base::test::TestFuture<scoped_refptr<os_crypt_async::Encryptor>> future;
    g_browser_process->os_crypt_async()->GetInstance(future.GetCallback());
    return future.Take();
  }

  // Makes an "Arc profile" from real Chromium data: this browser's cookie
  // database with a cookie for a.test, re-encrypted with Arc's key.
  void MakeArcProfileWithCookie() {
    ::Profile* profile = browser()->GetProfile();
    ASSERT_TRUE(content::SetCookie(profile, GURL("https://a.test/"),
                                   "session=1; max-age=3600"));
    base::test::TestFuture<void> flushed;
    profile->GetDefaultStoragePartition()
        ->GetCookieManagerForBrowserProcess()
        ->FlushCookieStore(flushed.GetCallback());
    ASSERT_TRUE(flushed.Wait());

    scoped_refptr<os_crypt_async::Encryptor> encryptor = GetEncryptor();
    const ChromiumMacCipher arc_cipher(kArcPassword);
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::CreateDirectory(arc_profile_));
    ASSERT_TRUE(base::CopyFile(profile->GetPath().AppendASCII("Cookies"),
                               arc_profile_.AppendASCII("Cookies")));
    sql::Database db(sql::Database::Tag("Test"));
    ASSERT_TRUE(db.Open(arc_profile_.AppendASCII("Cookies")));
    std::vector<std::pair<int64_t, std::string>> rows;
    {
      sql::Statement select(db.GetUniqueStatement(
          "SELECT rowid, encrypted_value FROM cookies"));
      while (select.Step()) {
        rows.emplace_back(select.ColumnInt64(0), select.ColumnBlobAsString(1));
      }
    }
    ASSERT_FALSE(rows.empty());
    for (const auto& [rowid, value] : rows) {
      std::string plaintext;
      ASSERT_TRUE(encryptor->DecryptString(value, &plaintext));
      sql::Statement update(db.GetUniqueStatement(
          "UPDATE cookies SET encrypted_value=? WHERE rowid=?"));
      update.BindBlob(0, arc_cipher.Encrypt(plaintext));
      update.BindInt64(1, rowid);
      ASSERT_TRUE(update.Run());
    }
  }

  // Arc's preferences: a protected one (the home page) with Arc's
  // signature, which isn't valid for Compass.
  void WriteArcPreferences() {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::CreateDirectory(arc_profile_));
    ASSERT_TRUE(base::WriteFile(arc_profile_.AppendASCII("Preferences"),
                                R"({"profile": {"name": "Arc name"},
            "protection": {"super_mac": "ARC"}})"));
    ASSERT_TRUE(base::WriteFile(arc_profile_.AppendASCII("Secure Preferences"),
                                R"({"homepage": "https://home.test/",
            "protection": {"macs": {"homepage": "ARCSIGNATURE"},
                           "super_mac": "ARC"}})"));
  }

  ProfileImport Import() {
    base::test::TestFuture<ProfileImport> future;
    ImportArcProfile("Default", u"Your Arc", future.GetCallback());
    return future.Take();
  }

  base::ScopedTempDir arc_dir_;
  base::FilePath arc_profile_;
};

IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, ImportsCookiesAndSettings) {
  ASSERT_NO_FATAL_FAILURE(MakeArcProfileWithCookie());
  ASSERT_NO_FATAL_FAILURE(WriteArcPreferences());

  ProfileImport result = Import();
  ASSERT_TRUE(result.profile);
  EXPECT_TRUE(result.copy.ok);
  EXPECT_GE(result.copy.reencrypted, 1);
  EXPECT_EQ(result.copy.dropped_cookies, 0);
  ::Profile* imported = result.profile;
  EXPECT_NE(imported, browser()->GetProfile());

  // Named as in Arc.
  ProfileAttributesEntry* entry =
      g_browser_process->profile_manager()
          ->GetProfileAttributesStorage()
          .GetProfileAttributesWithPath(imported->GetPath());
  ASSERT_TRUE(entry);
  EXPECT_EQ(entry->GetLocalProfileName(), u"Your Arc");

  // Still signed in: the cookie reads back with Compass's key.
  EXPECT_EQ(content::GetCookies(imported, GURL("https://a.test/")),
            "session=1");

  // The protected setting kept its value; nothing was reset.
  EXPECT_EQ(imported->GetPrefs()->GetString(prefs::kHomePage),
            "https://home.test/");
  EXPECT_TRUE(chrome_prefs::GetTamperedPrefList(imported).empty());
  EXPECT_TRUE(chrome_prefs::GetResetTime(imported).is_null());
}

IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, FailsWithoutArcsKey) {
  ASSERT_NO_FATAL_FAILURE(WriteArcPreferences());
  SetArcKeychainPasswordForTesting(std::optional<std::string>());
  // "No password" is what a refused Keychain prompt gives.
  ProfileImport result = Import();
  EXPECT_FALSE(result.profile);
  EXPECT_FALSE(result.copy.ok);
}

// Arc's sidebar for two profiles: "Your Arc" (Default) with two spaces and a
// favorite, "Work" (Profile 1) with one space.
constexpr char kSidebar[] = R"json({"sidebar": {"containers": [{"global": {}}, {
  "spaces": [
    "s1", {"id": "s1", "title": "Personal", "profile": {"default": true},
           "containerIDs": ["pinned", "p1", "unpinned", "u1"]},
    "s2", {"id": "s2", "title": "Job",
           "profile": {"custom": {"_0": {"directoryBasename": "Profile 1"}}},
           "containerIDs": ["pinned", "p2", "unpinned", "u2"]},
    "s3", {"id": "s3", "title": "Hobbies", "profile": {"default": true},
           "containerIDs": ["pinned", "p3", "unpinned", "u3"]}
  ],
  "items": [
    "p1", {"id": "p1", "data": {"itemContainer": {}}, "childrenIds": ["t1"]},
    "t1", {"id": "t1", "data": {"tab": {"savedURL": "https://news.test/",
           "savedTitle": "News"}}},
    "u1", {"id": "u1", "data": {"itemContainer": {}}, "childrenIds": []},
    "p2", {"id": "p2", "data": {"itemContainer": {}}, "childrenIds": []},
    "u2", {"id": "u2", "data": {"itemContainer": {}}, "childrenIds": ["t2"]},
    "t2", {"id": "t2", "data": {"tab": {"savedURL": "https://jira.test/"}}},
    "p3", {"id": "p3", "data": {"itemContainer": {}}, "childrenIds": []},
    "u3", {"id": "u3", "data": {"itemContainer": {}}, "childrenIds": []},
    "top1", {"id": "top1", "data": {"itemContainer": {}},
             "childrenIds": ["t3"]},
    "t3", {"id": "t3", "data": {"tab": {"savedURL": "https://mail.test/",
           "savedTitle": "Mail"}}}
  ],
  "topAppsContainerIDs": [{"default": true}, "top1"]
}]}})json";

constexpr char kLocalState[] = R"json({"profile": {"info_cache": {
  "Default": {"name": "Your Arc"}, "Profile 1": {"name": "Work"}}}})json";

void WriteArcSidebar() {
  base::ScopedAllowBlockingForTesting allow_blocking;
  const ArcPaths paths = GetArcPaths();
  for (const char* dir : {"Default", "Profile 1"}) {
    ASSERT_TRUE(base::CreateDirectory(paths.user_data.AppendASCII(dir)));
    ASSERT_TRUE(base::WriteFile(
        paths.user_data.AppendASCII(dir).AppendASCII("Preferences"), "{}"));
  }
  ASSERT_TRUE(base::WriteFile(paths.sidebar, kSidebar));
  ASSERT_TRUE(
      base::WriteFile(paths.user_data.AppendASCII("Local State"), kLocalState));
}

IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, ImportsProfilesAndSpaces) {
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    const ArcPaths paths = GetArcPaths();
    for (const char* dir : {"Default", "Profile 1"}) {
      ASSERT_TRUE(base::CreateDirectory(paths.user_data.AppendASCII(dir)));
      ASSERT_TRUE(base::WriteFile(
          paths.user_data.AppendASCII(dir).AppendASCII("Preferences"), "{}"));
    }
    ASSERT_TRUE(base::WriteFile(paths.sidebar, kSidebar));
    ASSERT_TRUE(base::WriteFile(paths.user_data.AppendASCII("Local State"),
                                kLocalState));
  }

  base::test::TestFuture<ArcImportResult> future;
  ImportFromArc({}, future.GetCallback());
  ArcImportResult result = future.Take();
  ASSERT_TRUE(result.ok);
  EXPECT_TRUE(result.failed.empty());
  ASSERT_EQ(result.profiles.size(), 2u);

  // "Your Arc": its two spaces (the first in the new profile's own place),
  // a pinned tab, and the favorite.
  ::Profile* personal = result.profiles[0];
  SpaceList& personal_spaces = SpaceService::Get(personal)->space_list();
  ASSERT_EQ(personal_spaces.spaces().size(), 2u);
  EXPECT_EQ(personal_spaces.spaces()[0]->name(), u"Personal");
  EXPECT_EQ(personal_spaces.spaces()[1]->name(), u"Hobbies");
  ASSERT_EQ(personal_spaces.spaces()[0]->pinned_root()->children().size(), 1u);
  EXPECT_EQ(personal_spaces.spaces()[0]->pinned_root()->children()[0]->url(),
            GURL("https://news.test/"));
  ASSERT_EQ(personal_spaces.favorites().pinned_root()->children().size(), 1u);

  // "Work": its space and Today tab.
  ::Profile* work = result.profiles[1];
  SpaceList& work_spaces = SpaceService::Get(work)->space_list();
  ASSERT_EQ(work_spaces.spaces().size(), 1u);
  EXPECT_EQ(work_spaces.spaces()[0]->name(), u"Job");
  EXPECT_EQ(work_spaces.spaces()[0]->today_root()->children().size(), 1u);

  // Both show in the switcher.
  EXPECT_TRUE(SpaceDirectory::Get()->IsListed(personal));
  EXPECT_TRUE(SpaceDirectory::Get()->IsListed(work));
}

IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, NoSidebarNoImport) {
  base::test::TestFuture<ArcImportResult> future;
  ImportFromArc({}, future.GetCallback());
  ArcImportResult result = future.Take();
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.profiles.empty());
}

// The page lists Arc's profiles, imports the chosen ones, and says so.
IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, ImportPage) {
  ASSERT_NO_FATAL_FAILURE(WriteArcSidebar());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://compass-import/")));
  content::WebContents* page =
      browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(page));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page, "document.querySelectorAll('#profiles li')"
                                 ".length")
               .ExtractInt() == 2;
  }));
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelector('#profiles .details')"
                            ".textContent"),
            "2 spaces (Personal, Hobbies) · 2 tabs");
  EXPECT_EQ(false, content::EvalJs(page, "document.getElementById('import')"
                                         ".disabled"));

  // Only "Work".
  ASSERT_TRUE(content::ExecJs(
      page, "document.querySelector('#profiles input').click();"
            "document.getElementById('import').click();"));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page, "!document.getElementById('done').hidden")
        .ExtractBool();
  }));
  EXPECT_EQ(content::EvalJs(page,
                            "document.getElementById('doneText').textContent"),
            "Imported Work.");

  // Opened again, Work shows as imported.
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://compass-import/")));
  page = browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page, "document.querySelectorAll('#profiles li')"
                                 ".length")
               .ExtractInt() == 2;
  }));
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelectorAll('#profiles .name')[1]"
                            ".textContent"),
            "Work (imported)");
}

}  // namespace
}  // namespace compass::arc
