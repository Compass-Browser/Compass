// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/arc_import/arc_importer.h"

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
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
#include "build/build_config.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "compass/browser/browser_import/chromium_crypto.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/sidebar/sidebar_node.h"
#include "compass/browser/sidebar/space_list.h"
#include "compass/browser/sidebar/space_model.h"
#include "compass/browser/ui/views/settings_window/settings_window.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "net/cookies/canonical_cookie.h"
#include "net/cookies/cookie_constants.h"
#include "net/extras/sqlite/cookie_crypto_delegate.h"
#include "net/extras/sqlite/sqlite_persistent_cookie_store.h"
#include "net/log/net_log_with_source.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "sql/database.h"
#include "sql/statement.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace compass::arc {
namespace {

// Arc's key: macOS, its Keychain password; Windows, the AES key its Local
// State keeps DPAPI-wrapped.
constexpr char kArcPassword[] = "arc test password";
#if BUILDFLAG(IS_WIN)
constexpr char kArcWindowsKey[] = "arc test key, 32 bytes, for GCM.";
static_assert(sizeof(kArcWindowsKey) - 1 == ChromiumWindowsCipher::kKeySize);
#endif

// |plaintext| encrypted as Arc does on this platform.
std::string ArcEncrypt(std::string_view plaintext) {
#if BUILDFLAG(IS_WIN)
  return ChromiumWindowsCipher(kArcWindowsKey).Encrypt(plaintext);
#else
  return ChromiumMacCipher(kArcPassword).Encrypt(plaintext);
#endif
}

// Writes Arc's Local State |json|, with its key on Windows.
void WriteArcLocalState(std::string_view json) {
  base::ScopedAllowBlockingForTesting allow_blocking;
  std::optional<base::Value> local_state = base::JSONReader::Read(
      json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  ASSERT_TRUE(local_state && local_state->is_dict());
#if BUILDFLAG(IS_WIN)
  const std::string wrapped =
      browser_import::WrapWindowsKeyForTesting(kArcWindowsKey);
  ASSERT_FALSE(wrapped.empty());
  local_state->GetDict().SetByDottedPath("os_crypt.encrypted_key", wrapped);
#endif
  const base::FilePath user_data = GetArcPaths().user_data;
  ASSERT_TRUE(base::CreateDirectory(user_data));
  ASSERT_TRUE(base::WriteFile(user_data.AppendASCII("Local State"),
                              *base::WriteJson(*local_state)));
}

// Encrypts the cookies Chromium's cookie store writes as Arc would.
class ArcCookieCrypto : public net::CookieCryptoDelegate {
 public:
  void Init(base::OnceClosure callback) override { std::move(callback).Run(); }
  bool EncryptString(const std::string& plaintext,
                     std::string* ciphertext) override {
    *ciphertext = ArcEncrypt(plaintext);
    return true;
  }
  bool DecryptString(const std::string& ciphertext,
                     std::string* plaintext) override {
    return false;
  }
};

class ArcImporterBrowserTest : public InProcessBrowserTest {
 protected:
  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    ASSERT_TRUE(arc_dir_.CreateUniqueTempDir());
    ArcPaths paths;
    paths.user_data = arc_dir_.GetPath().AppendASCII("User Data");
    paths.cache = arc_dir_.GetPath().AppendASCII("Caches");
    paths.sidebar = arc_dir_.GetPath().AppendASCII("StorableSidebar.json");
    paths.archive =
        arc_dir_.GetPath().AppendASCII("StorableArchiveItems.json");
    SetArcPathsForTesting(paths);
    SetArcKeychainPasswordForTesting(kArcPassword);
    SetArcRunningForTesting(false);
    arc_profile_ = paths.user_data.AppendASCII("Default");
    ASSERT_NO_FATAL_FAILURE(WriteArcLocalState("{}"));
  }

  void TearDownOnMainThread() override {
    SetArcPathsForTesting(std::nullopt);
    SetArcKeychainPasswordForTesting(std::nullopt);
    SetArcRunningForTesting(std::nullopt);
    InProcessBrowserTest::TearDownOnMainThread();
  }

  // Makes an "Arc profile" with real Chromium data: a cookie database with
  // a cookie for a.test, written by Chromium's cookie store with Arc's key,
  // where this platform keeps it (Windows: in "Network"). This browser's own
  // database can't be copied on Windows: the network service holds it
  // locked.
  void MakeArcProfileWithCookie() {
#if BUILDFLAG(IS_WIN)
    const base::FilePath arc_cookies =
        arc_profile_.AppendASCII("Network").AppendASCII("Cookies");
#else
    const base::FilePath arc_cookies = arc_profile_.AppendASCII("Cookies");
#endif
    {
      base::ScopedAllowBlockingForTesting allow_blocking;
      ASSERT_TRUE(base::CreateDirectory(arc_cookies.DirName()));
    }
    scoped_refptr<base::SequencedTaskRunner> background =
        base::ThreadPool::CreateSequencedTaskRunner({base::MayBlock()});
    auto store = base::MakeRefCounted<net::SQLitePersistentCookieStore>(
        arc_cookies, base::SequencedTaskRunner::GetCurrentDefault(),
        background, /*restore_old_session_cookies=*/false,
        std::make_unique<ArcCookieCrypto>(),
        /*enable_exclusive_access=*/false);
    base::test::TestFuture<std::vector<std::unique_ptr<net::CanonicalCookie>>>
        loaded;
    store->Load(loaded.GetCallback(), net::NetLogWithSource());
    ASSERT_TRUE(loaded.Wait());
    std::unique_ptr<net::CanonicalCookie> cookie =
        net::CanonicalCookie::CreateForTesting(
            GURL("https://a.test/"), "session=1; max-age=3600",
            base::Time::Now(), net::CookieSourceType::kHTTP);
    ASSERT_TRUE(cookie);
    store->AddCookie(*cookie);
    base::test::TestFuture<void> flushed;
    store->Flush(flushed.GetCallback());
    ASSERT_TRUE(flushed.Wait());
    // Closed on the background sequence.
    store = nullptr;
    base::RunLoop closed;
    background->PostTask(FROM_HERE, closed.QuitClosure());
    closed.Run();
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
  EXPECT_EQ(imported->GetPrefs()->GetString(::prefs::kHomePage),
            "https://home.test/");
  EXPECT_TRUE(chrome_prefs::GetTamperedPrefList(imported).empty());
  EXPECT_TRUE(chrome_prefs::GetResetTime(imported).is_null());
}

IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, FailsWithoutArcsKey) {
  ASSERT_NO_FATAL_FAILURE(WriteArcPreferences());
  SetArcKeychainPasswordForTesting(std::optional<std::string>());
  // "No password" is what a refused Keychain prompt gives; on Windows, a
  // Local State without the key.
  {
    base::ScopedAllowBlockingForTesting allow_blocking;
    ASSERT_TRUE(base::WriteFile(
        GetArcPaths().user_data.AppendASCII("Local State"), "{}"));
  }
  ProfileImport result = Import();
  EXPECT_FALSE(result.profile);
  EXPECT_FALSE(result.copy.ok);
}

// Arc's sidebar for two profiles: "Your Arc" (Default) with two spaces and a
// favorite, "Work" (Profile 1) with one space. More spaces go in for the
// "%s" (WriteArcSidebar()).
constexpr char kSidebar[] = R"json({"sidebar": {"containers": [{"global": {}}, {
  "spaces": [%s
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

// A space of "Your Arc" added later, for importing again.
constexpr char kTravelSpace[] = R"json(
    "s4", {"id": "s4", "title": "Travel", "profile": {"default": true},
           "containerIDs": ["pinned", "p3", "unpinned", "u3"]},)json";

// Writes Arc's sidebar, with |more_spaces| first.
void WriteArcSidebar(std::string_view more_spaces = "") {
  base::ScopedAllowBlockingForTesting allow_blocking;
  const ArcPaths paths = GetArcPaths();
  for (const char* dir : {"Default", "Profile 1"}) {
    ASSERT_TRUE(base::CreateDirectory(paths.user_data.AppendASCII(dir)));
    ASSERT_TRUE(base::WriteFile(
        paths.user_data.AppendASCII(dir).AppendASCII("Preferences"), "{}"));
  }
  std::string sidebar = kSidebar;
  sidebar.replace(sidebar.find("%s"), 2, more_spaces);
  ASSERT_TRUE(base::WriteFile(paths.sidebar, sidebar));
  // A tab archived from "Personal".
  ASSERT_TRUE(base::WriteFile(paths.archive, R"json({"items": [
    "a1", {"sidebarItem": {"id": "a1", "data": {"tab": {
             "savedURL": "https://archived.test/", "savedTitle": "Gone"}}},
           "archivedAt": 100, "source": {"space": {"_0": "s1"}}}]})json"));
  ASSERT_NO_FATAL_FAILURE(WriteArcLocalState(kLocalState));
}

IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, ImportsProfilesAndSpaces) {
  ASSERT_NO_FATAL_FAILURE(WriteArcSidebar());

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
  ASSERT_EQ(personal_spaces.spaces()[0]->archive().size(), 1u);
  EXPECT_EQ(personal_spaces.spaces()[0]->archive()[0].tabs[0].title, u"Gone");

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

std::vector<std::u16string> SpaceNames(::Profile* profile) {
  std::vector<std::u16string> names;
  for (const auto& space : SpaceService::Get(profile)->space_list().spaces()) {
    names.push_back(space->name());
  }
  return names;
}

// Only the chosen profiles and spaces come in, and the profile records
// which spaces it got.
IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, ImportsChosenSpacesOnly) {
  ASSERT_NO_FATAL_FAILURE(WriteArcSidebar());

  ArcImportChoice choice;
  choice.profiles = std::vector<std::string>{"Default"};
  choice.spaces = std::set<std::string>{"s3"};
  base::test::TestFuture<ArcImportResult> future;
  ImportFromArc(choice, future.GetCallback());
  ArcImportResult result = future.Take();
  ASSERT_TRUE(result.ok);
  ASSERT_EQ(result.profiles.size(), 1u);
  EXPECT_THAT(SpaceNames(result.profiles[0]),
              testing::ElementsAre(u"Hobbies"));
  EXPECT_EQ(result.profiles[0]->GetPrefs()->GetList(prefs::kArcImportSpaces),
            base::ListValue().Append("s3"));
}

// Importing again adds only Arc's newer spaces, into the profile made the
// first time; no new profile, no second copy of the favorites.
IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, ImportingAgainAddsNewSpaces) {
  ASSERT_NO_FATAL_FAILURE(WriteArcSidebar());
  base::test::TestFuture<ArcImportResult> first;
  ImportFromArc({}, first.GetCallback());
  ArcImportResult result = first.Take();
  ASSERT_EQ(result.profiles.size(), 2u);
  ::Profile* personal = result.profiles[0];
  const size_t profiles = g_browser_process->profile_manager()
                              ->GetProfileAttributesStorage()
                              .GetNumberOfProfiles();

  // Nothing new: nothing happens.
  base::test::TestFuture<ArcImportResult> again;
  ImportFromArc({}, again.GetCallback());
  result = again.Take();
  EXPECT_TRUE(result.profiles.empty());
  EXPECT_TRUE(result.updated.empty());

  // A new space in Arc.
  ASSERT_NO_FATAL_FAILURE(WriteArcSidebar(kTravelSpace));
  base::test::TestFuture<ArcImportResult> newer;
  ImportFromArc({}, newer.GetCallback());
  result = newer.Take();
  ASSERT_TRUE(result.ok);
  EXPECT_TRUE(result.profiles.empty());
  ASSERT_EQ(result.updated.size(), 1u);
  EXPECT_EQ(result.updated[0], personal);
  EXPECT_THAT(SpaceNames(personal),
              testing::ElementsAre(u"Personal", u"Hobbies", u"Travel"));
  EXPECT_EQ(SpaceService::Get(personal)
                ->space_list()
                .favorites()
                .pinned_root()
                ->children()
                .size(),
            1u);
  EXPECT_EQ(g_browser_process->profile_manager()
                ->GetProfileAttributesStorage()
                .GetNumberOfProfiles(),
            profiles);
}

// On the first run, with Arc on the Mac, the first window opens the import,
// in the settings window's pane.
IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, OffersImportOnFirstRun) {
  ASSERT_NO_FATAL_FAILURE(WriteArcSidebar());
  SetFirstRunForArcImportOfferForTesting(true);
  CreateBrowser(browser()->GetProfile());
  ASSERT_TRUE(base::test::RunUntil([&] {
    SettingsWindow* settings =
        SettingsWindow::FromProfile(browser()->GetProfile());
    return settings && settings->web_contents() &&
           settings->web_contents()->GetVisibleURL().path() == "/importArc";
  }));
  SetFirstRunForArcImportOfferForTesting(std::nullopt);
}

IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, NoSidebarNoImport) {
  base::test::TestFuture<ArcImportResult> future;
  ImportFromArc({}, future.GetCallback());
  ArcImportResult result = future.Take();
  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.profiles.empty());
}

// The page lists Arc with the other browsers; its row opens Arc's
// profiles, which imports the chosen ones, and says so.
IN_PROC_BROWSER_TEST_F(ArcImporterBrowserTest, ImportPage) {
  ASSERT_NO_FATAL_FAILURE(WriteArcSidebar());
  ASSERT_TRUE(ui_test_utils::NavigateToURL(
      browser(), GURL("chrome://compass-import/")));
  content::WebContents* page =
      browser()->GetTabStripModel()->GetActiveWebContents();
  ASSERT_TRUE(content::WaitForLoadStop(page));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return content::EvalJs(page, "document.querySelectorAll('#profiles > li')"
                                 ".length")
               .ExtractInt() == 2;
  }));
  EXPECT_EQ(false, content::EvalJs(
                       page, "document.getElementById('arcGroup').hidden"));
  EXPECT_EQ(content::EvalJs(page,
                            "document.getElementById('arcDetail')"
                            ".textContent"),
            "2 profiles · 3 spaces");
  ASSERT_TRUE(
      content::ExecJs(page, "document.getElementById('arcRow').click();"));
  EXPECT_EQ(false, content::EvalJs(
                       page, "document.getElementById('arcView').hidden"));
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelector('#profiles .details')"
                            ".textContent"),
            "2 spaces · 2 tabs");
  EXPECT_EQ(content::EvalJs(page,
                            "[...document.querySelectorAll("
                            "'#profiles .space-name')].map(e => e.textContent)"
                            ".join()"),
            "Personal,Hobbies,Job");
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelector('#profiles .spaces "
                            ".details').textContent"),
            "1 tab · 1 archived");
  EXPECT_EQ(false, content::EvalJs(page, "document.getElementById('import')"
                                         ".disabled"));

  // Only "Work".
  ASSERT_TRUE(content::ExecJs(
      page, "document.querySelector('#profiles .profile-check').click();"
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
    return content::EvalJs(page, "document.querySelectorAll('#profiles > li')"
                                 ".length")
               .ExtractInt() == 2;
  }));
  EXPECT_EQ(content::EvalJs(page,
                            "document.querySelectorAll('#profiles .name')[1]"
                            ".textContent"),
            "Work (imported)");
  // Its space was brought in: nothing to choose there.
  EXPECT_EQ(true, content::EvalJs(
                      page, "document.querySelectorAll('#profiles "
                            ".space-check')[2].disabled"));
}

}  // namespace
}  // namespace compass::arc
