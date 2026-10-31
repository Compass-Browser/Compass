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
#include "compass/browser/arc_import/arc_crypto.h"
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
    paths.user_data = arc_dir_.GetPath().Append("User Data");
    paths.cache = arc_dir_.GetPath().Append("Caches");
    paths.sidebar = arc_dir_.GetPath().Append("StorableSidebar.json");
    SetArcPathsForTesting(paths);
    SetArcKeychainPasswordForTesting(kArcPassword);
    arc_profile_ = paths.user_data.Append("Default");
  }

  void TearDownOnMainThread() override {
    SetArcPathsForTesting(std::nullopt);
    SetArcKeychainPasswordForTesting(std::nullopt);
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
    Profile* profile = browser()->GetProfile();
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
    ASSERT_TRUE(base::CopyFile(profile->GetPath().Append("Cookies"),
                               arc_profile_.Append("Cookies")));
    sql::Database db(sql::Database::Tag("Test"));
    ASSERT_TRUE(db.Open(arc_profile_.Append("Cookies")));
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
    ASSERT_TRUE(base::WriteFile(
        arc_profile_.Append("Preferences"),
        R"({"profile": {"name": "Arc name"},
            "protection": {"super_mac": "ARC"}})"));
    ASSERT_TRUE(base::WriteFile(
        arc_profile_.Append("Secure Preferences"),
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
  Profile* imported = result.profile;
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

}  // namespace
}  // namespace compass::arc
