// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/arc_import/arc_importer.h"

#include <algorithm>
#include <utility>

#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/json/json_file_value_serializer.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/first_run/first_run.h"
#include "chrome/browser/prefs/chrome_pref_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_paths_internal.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "compass/browser/arc_import/arc_easels.h"
#include "compass/browser/arc_import/sidebar_import.h"
#include "compass/browser/browser_import/chromium_crypto.h"
#include "compass/browser/browser_import/detect.h"
#include "compass/browser/prefs/compass_prefs.h"
#include "compass/browser/ui/arc_import/arc_easel_import.h"
#include "compass/browser/ui/webui/settings/settings_sections.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "compass/browser/window_sync/window_features.h"
#include "components/prefs/pref_service.h"
#include "components/prefs/scoped_user_pref_update.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace compass::arc {

namespace {

constexpr base::FilePath::CharType kSecurePreferencesFilename[] =
    FILE_PATH_LITERAL("Secure Preferences");
// Arc's and Compass's signatures of protected preferences.
constexpr char kProtectionKey[] = "protection";

std::optional<ArcPaths>& PathsForTesting() {
  static base::NoDestructor<std::optional<ArcPaths>> paths;
  return *paths;
}

std::optional<base::DictValue> ReadJsonDict(const base::FilePath& path) {
  JSONFileValueDeserializer deserializer(path);
  std::unique_ptr<base::Value> value =
      deserializer.Deserialize(nullptr, nullptr);
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  return std::move(*value).TakeDict();
}

// Copies the profile on a blocking thread: finds Arc, reads its key, then
// copies and re-encrypts with Compass's |encryptor|.
CopyResult CopyOnPool(const std::string& arc_dir,
                      const base::FilePath& to,
                      const base::FilePath& to_cache,
                      scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  const ArcPaths arc = GetArcPaths();
  if (arc.user_data.empty()) {
    return CopyResult();
  }
  ArcDecryptor decrypt = GetArcDecryptor(arc.user_data);
  if (!decrypt) {
    LOG(ERROR) << "Arc import: no access to Arc's key";
    return CopyResult();
  }
  // Where the cache is in the profile (Windows), it comes with it.
  const base::FilePath from_cache =
      arc.cache.empty() ? base::FilePath() : arc.cache.AppendASCII(arc_dir);
  CopyResult result = CopyProfile(
      arc.user_data.AppendASCII(arc_dir), to, from_cache, to_cache,
      base::BindRepeating(
          [](const ArcDecryptor& decrypt,
             const os_crypt_async::Encryptor* encryptor,
             std::string_view value) -> std::optional<std::string> {
            std::optional<std::string> plaintext = decrypt.Run(value);
            std::string ciphertext;
            if (!plaintext ||
                !encryptor->EncryptString(*plaintext, &ciphertext)) {
              return std::nullopt;
            }
            return ciphertext;
          },
          decrypt, base::RetainedRef(encryptor)));
  if (result.ok && !SignImportedPreferences(to, arc_dir)) {
    base::DeletePathRecursively(to);
    return CopyResult();
  }
  return result;
}

void OnProfileLoaded(std::u16string name,
                     CopyResult copy,
                     base::OnceCallback<void(ProfileImport)> callback,
                     ::Profile* profile) {
  if (profile) {
    if (ProfileAttributesEntry* entry =
            g_browser_process->profile_manager()
                ->GetProfileAttributesStorage()
                .GetProfileAttributesWithPath(profile->GetPath())) {
      entry->SetLocalProfileName(name, /*is_default_name=*/false);
    }
  }
  std::move(callback).Run({profile, copy});
}

void OnCopied(const base::FilePath& to,
              std::u16string name,
              base::OnceCallback<void(ProfileImport)> callback,
              CopyResult copy) {
  if (!copy.ok) {
    std::move(callback).Run({nullptr, copy});
    return;
  }
  g_browser_process->profile_manager()->CreateProfileAsync(
      to, base::BindOnce(&OnProfileLoaded, std::move(name), copy,
                         std::move(callback)));
}

void StartCopy(const std::string& arc_dir,
               std::u16string name,
               base::OnceCallback<void(ProfileImport)> callback,
               scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  const base::FilePath to = g_browser_process->profile_manager()
                                ->GenerateNextProfileDirectoryPath();
  base::FilePath to_cache;
  chrome::GetUserCacheDirectory(to, &to_cache);
  // Sequenced: signing the preferences needs a sequence (PrefHashFilter).
  base::ThreadPool::CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
       base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN})
      ->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&CopyOnPool, arc_dir, to, to_cache,
                     std::move(encryptor)),
      base::BindOnce(&OnCopied, to, std::move(name),
                     std::move(callback)));
}

// Reads Arc's sidebar, its profiles' names and its archive. Blocking.
std::optional<Sidebar> ReadSidebar(const ArcPaths& paths) {
  std::optional<base::DictValue> sidebar = ReadJsonDict(paths.sidebar);
  if (!sidebar) {
    return std::nullopt;
  }
  std::optional<base::DictValue> local_state =
      ReadJsonDict(paths.user_data.Append(chrome::kLocalStateFilename));
  std::optional<Sidebar> parsed =
      ParseSidebar(*sidebar, local_state ? &*local_state : nullptr);
  // The archive is optional: without it, spaces come with an empty one.
  if (parsed && !paths.archive.empty()) {
    if (std::optional<base::DictValue> archive =
            ReadJsonDict(paths.archive)) {
      AddArchive(ParseArchive(*archive), parsed->spaces);
    }
  }
  return parsed;
}

// One step of an import: an Arc profile and the spaces to bring in, into a
// new profile or into the one it was imported as before.
struct Job {
  Profile arc;
  // The Compass profile it was imported as before; empty: a new one.
  base::FilePath existing;
  std::vector<Space> spaces;
  // The boards |spaces| point at.
  std::vector<ArcEaselRef> easels;
  // Only |easels|, into |existing| (ImportArcEaselsAgain()).
  bool easels_only = false;
};

// What |choice| brings in from |sidebar|, given what's |imported| already.
std::vector<Job> PlanImport(const Sidebar& sidebar,
                            const std::map<std::string, ImportedProfile>&
                                imported,
                            const ArcImportChoice& choice) {
  std::vector<Job> jobs;
  for (const Profile& arc_profile : sidebar.profiles) {
    const auto before = imported.find(arc_profile.dir);
    Job job;
    job.arc = arc_profile;
    for (const Space& space : sidebar.spaces) {
      if (space.profile_dir != arc_profile.dir ||
          (choice.spaces && !choice.spaces->contains(space.id)) ||
          (before != imported.end() &&
           before->second.spaces.contains(space.id))) {
        continue;
      }
      job.spaces.push_back(space);
    }
    if (before != imported.end()) {
      // Imported before: only its newer spaces, if any were chosen.
      if (job.spaces.empty()) {
        continue;
      }
      job.existing = before->second.path;
    } else if (choice.profiles &&
               !std::ranges::contains(*choice.profiles, arc_profile.dir)) {
      continue;
    }
    job.easels = EaselsIn(job.spaces);
    jobs.push_back(std::move(job));
  }
  return jobs;
}

// One import from Arc: its jobs one after another. Owns itself until it
// reports.
class WholeImport {
 public:
  WholeImport(std::vector<Job> jobs,
              scoped_refptr<SharedArcEasels> easels,
              base::OnceCallback<void(ArcImportResult)> callback)
      : jobs_(std::move(jobs)),
        easels_(std::move(easels)),
        callback_(std::move(callback)) {
    result_.ok = true;
  }

  void Next() {
    if (next_ >= jobs_.size()) {
      std::move(callback_).Run(std::move(result_));
      delete this;
      return;
    }
    const Job& job = jobs_[next_];
    if (!job.existing.empty()) {
      // Loads it if it isn't (imported profiles are listed, so usually are).
      g_browser_process->profile_manager()->CreateProfileAsync(
          job.existing, base::BindOnce(&WholeImport::OnProfileReady,
                                       weak_factory_.GetWeakPtr()));
      return;
    }
    ImportArcProfile(job.arc.dir, job.arc.name,
                     base::BindOnce(&WholeImport::OnProfileImported,
                                    weak_factory_.GetWeakPtr()));
  }

 private:
  void OnProfileImported(ProfileImport import) {
    if (import.profile) {
      result_.undecryptable +=
          import.copy.dropped_cookies + import.copy.app_bound;
    }
    OnProfileReady(import.profile);
  }

  void OnProfileReady(::Profile* profile) {
    const Job& job = jobs_[next_];
    if (!profile) {
      result_.failed.push_back(job.arc.dir);
      ++next_;
      Next();
      return;
    }
    if (job.easels_only) {
      ImportEasels(profile->GetWeakPtr(),
                   base::BindOnce(&WholeImport::OnCommitted,
                                  weak_factory_.GetWeakPtr()));
      return;
    }
    (job.existing.empty() ? result_.profiles : result_.updated)
        .push_back(profile);
    SpaceService* service = SpaceService::Get(profile);
    service->RunWhenLoaded(base::BindOnce(&WholeImport::OnSpacesLoaded,
                                          weak_factory_.GetWeakPtr(),
                                          profile->GetWeakPtr()));
  }

  void OnSpacesLoaded(base::WeakPtr<::Profile> profile) {
    const Job& job = jobs_[next_];
    if (!profile) {
      ++next_;
      Next();
      return;
    }
    std::vector<SpaceModel*> added =
        AddArcSidebar(SpaceService::Get(profile.get())->space_list(), job.arc,
                      job.spaces, base::Time::Now(),
                      job.existing.empty() ? ArcImportInto::kNewProfile
                                           : ArcImportInto::kImportedProfile);
    if (!added.empty() && !result_.first_space.is_valid()) {
      result_.first_space = added.front()->id();
    }
    PrefService* pref_service = profile->GetPrefs();
    {
      ScopedListPrefUpdate imported(pref_service, prefs::kArcImportSpaces);
      for (const Space& space : job.spaces) {
        if (!imported->contains(space.id)) {
          imported->Append(space.id);
        }
      }
    }
    // Its spaces show in every window's switcher.
    if (SpaceDirectory* directory = SpaceDirectory::Get()) {
      directory->List(profile.get());
    }
    // The boards its easel tabs open.
    ImportEasels(profile, base::BindOnce(&WholeImport::Commit,
                                         weak_factory_.GetWeakPtr(), profile));
  }

  void ImportEasels(base::WeakPtr<::Profile> profile, base::OnceClosure then) {
    const Job& job = jobs_[next_];
    if (!profile || job.easels.empty()) {
      std::move(then).Run();
      return;
    }
    ImportArcEasels(profile.get(), job.easels, easels_,
                    base::BindOnce(&WholeImport::OnEaselsImported,
                                   weak_factory_.GetWeakPtr(),
                                   std::move(then)));
  }

  void OnEaselsImported(base::OnceClosure then, ArcEaselImportCounts counts) {
    result_.easels += counts.imported;
    result_.easels_missing += counts.missing;
    std::move(then).Run();
  }

  void Commit(base::WeakPtr<::Profile> profile) {
    if (!profile) {
      OnCommitted();
      return;
    }
    // On disk before the page asks what was imported.
    profile->GetPrefs()->CommitPendingWrite(base::BindOnce(
        &WholeImport::OnCommitted, weak_factory_.GetWeakPtr()));
  }

  void OnCommitted() {
    ++next_;
    Next();
  }

  std::vector<Job> jobs_;
  // Arc's boards, when a job has any.
  const scoped_refptr<SharedArcEasels> easels_;
  base::OnceCallback<void(ArcImportResult)> callback_;
  ArcImportResult result_;
  size_t next_ = 0;
  base::WeakPtrFactory<WholeImport> weak_factory_{this};
};

// The Compass profiles under |user_data| imported from Arc, by Arc profile
// folder (the first one if two came from the same). Blocking.
std::map<std::string, ImportedProfile> FindImported(
    const base::FilePath& user_data) {
  std::map<std::string, ImportedProfile> imported;
  base::FileEnumerator profiles(user_data, /*recursive=*/false,
                                base::FileEnumerator::DIRECTORIES);
  for (base::FilePath dir = profiles.Next(); !dir.empty();
       dir = profiles.Next()) {
    std::optional<base::DictValue> preferences =
        ReadJsonDict(dir.Append(chrome::kPreferencesFilename));
    const std::string* from =
        preferences
            ? preferences->FindStringByDottedPath(prefs::kArcImportProfileDir)
            : nullptr;
    if (!from || from->empty() || imported.contains(*from)) {
      continue;
    }
    ImportedProfile& profile = imported[*from];
    profile.path = dir;
    if (const base::ListValue* spaces =
            preferences->FindListByDottedPath(prefs::kArcImportSpaces)) {
      for (const base::Value& space : *spaces) {
        if (space.is_string()) {
          profile.spaces.insert(space.GetString());
        }
      }
    }
  }
  return imported;
}

ArcOverview ReadOverview(const base::FilePath& user_data) {
  ArcOverview overview;
  overview.sidebar = ReadSidebar(GetArcPaths());
  overview.imported = FindImported(user_data);
  return overview;
}

// Leaves out profiles the user deleted (their folders go later).
ArcOverview DropDeleted(ArcOverview overview) {
  ProfileAttributesStorage& storage =
      g_browser_process->profile_manager()->GetProfileAttributesStorage();
  std::erase_if(overview.imported, [&](const auto& imported) {
    return !storage.GetProfileAttributesWithPath(imported.second.path);
  });
  return overview;
}

// Arc's boards, from its Firestore cache. Blocking.
scoped_refptr<SharedArcEasels> ReadEasels() {
  auto easels = base::MakeRefCounted<SharedArcEasels>();
  easels->data = ReadArcEasels(GetArcPaths().easels);
  return easels;
}

// Runs |jobs|, after reading Arc's boards if any job has some.
void StartImport(std::vector<Job> jobs,
                 base::OnceCallback<void(ArcImportResult)> callback) {
  auto start = [](std::vector<Job> jobs,
                  base::OnceCallback<void(ArcImportResult)> callback,
                  scoped_refptr<SharedArcEasels> easels) {
    (new WholeImport(std::move(jobs), std::move(easels), std::move(callback)))
        ->Next();
  };
  if (std::ranges::none_of(
          jobs, [](const Job& job) { return !job.easels.empty(); })) {
    start(std::move(jobs), std::move(callback), nullptr);
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadEasels),
      base::BindOnce(start, std::move(jobs), std::move(callback)));
}

void OnOverviewForImport(ArcImportChoice choice,
                         base::OnceCallback<void(ArcImportResult)> callback,
                         ArcOverview overview) {
  overview = DropDeleted(std::move(overview));
  if (!overview.sidebar) {
    std::move(callback).Run(ArcImportResult());
    return;
  }
  StartImport(PlanImport(*overview.sidebar, overview.imported, choice),
              std::move(callback));
}

// The boards of the Arc spaces each imported profile has.
void OnOverviewForEasels(base::OnceCallback<void(ArcImportResult)> callback,
                         ArcOverview overview) {
  overview = DropDeleted(std::move(overview));
  if (!overview.sidebar) {
    std::move(callback).Run(ArcImportResult());
    return;
  }
  std::vector<Job> jobs;
  for (const auto& [dir, imported] : overview.imported) {
    std::vector<Space> spaces;
    for (const Space& space : overview.sidebar->spaces) {
      if (space.profile_dir == dir && imported.spaces.contains(space.id)) {
        spaces.push_back(space);
      }
    }
    Job job;
    job.arc.dir = dir;
    job.existing = imported.path;
    job.easels = EaselsIn(spaces);
    job.easels_only = true;
    if (!job.easels.empty()) {
      jobs.push_back(std::move(job));
    }
  }
  StartImport(std::move(jobs), std::move(callback));
}

void ReadOverviewThen(base::OnceCallback<void(ArcOverview)> callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadOverview,
                     g_browser_process->profile_manager()->user_data_dir()),
      std::move(callback));
}

bool g_offer_considered = false;
std::optional<bool> g_first_run_for_testing;

// Whether Arc's sidebar exists and nothing was imported from it yet, or
// another browser has profiles (docs/design/browser-import.md). Blocking.
bool ShouldOffer(const base::FilePath& user_data) {
  const ArcPaths paths = GetArcPaths();
  if (!paths.sidebar.empty() && base::PathExists(paths.sidebar) &&
      FindImported(user_data).empty()) {
    return true;
  }
  return !browser_import::DetectProfiles().empty();
}

}  // namespace

ArcImportResult::ArcImportResult() = default;
ArcImportResult::ArcImportResult(ArcImportResult&&) = default;
ArcImportResult& ArcImportResult::operator=(ArcImportResult&&) = default;
ArcImportResult::~ArcImportResult() = default;

void ImportFromArc(ArcImportChoice choice,
                   base::OnceCallback<void(ArcImportResult)> callback) {
  ReadOverviewThen(base::BindOnce(&OnOverviewForImport, std::move(choice),
                                  std::move(callback)));
}

void ImportArcEaselsAgain(
    base::OnceCallback<void(ArcImportResult)> callback) {
  ReadOverviewThen(
      base::BindOnce(&OnOverviewForEasels, std::move(callback)));
}

ArcImportChoice::ArcImportChoice() = default;
ArcImportChoice::ArcImportChoice(const ArcImportChoice&) = default;
ArcImportChoice& ArcImportChoice::operator=(const ArcImportChoice&) = default;
ArcImportChoice::~ArcImportChoice() = default;

ImportedProfile::ImportedProfile() = default;
ImportedProfile::ImportedProfile(const ImportedProfile&) = default;
ImportedProfile& ImportedProfile::operator=(const ImportedProfile&) = default;
ImportedProfile::~ImportedProfile() = default;

ArcOverview::ArcOverview() = default;
ArcOverview::ArcOverview(ArcOverview&&) = default;
ArcOverview& ArcOverview::operator=(ArcOverview&&) = default;
ArcOverview::~ArcOverview() = default;

void GetArcOverview(base::OnceCallback<void(ArcOverview)> callback) {
  ReadOverviewThen(base::BindOnce(&DropDeleted).Then(std::move(callback)));
}

void MaybeOfferImportOnFirstRun(BrowserWindowInterface* browser) {
  if (g_offer_considered) {
    return;
  }
  WindowFeatures* features = WindowFeatures::From(browser);
  if (browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      browser->GetProfile()->IsOffTheRecord() || !features ||
      features->is_parking_window()) {
    return;
  }
  // The first window the user sees decides.
  g_offer_considered = true;
  if (!g_first_run_for_testing.value_or(first_run::IsChromeFirstRun())) {
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ShouldOffer,
                     g_browser_process->profile_manager()->user_data_dir()),
      base::BindOnce(
          [](base::WeakPtr<BrowserWindowInterface> browser, bool offer) {
            if (browser && offer) {
              chrome::ShowSettingsSubPage(browser.get(),
                                          kArcImportSettingsSubPage);
            }
          },
          browser->GetWeakPtr()));
}

void SetFirstRunForArcImportOfferForTesting(std::optional<bool> first_run) {
  g_offer_considered = false;
  g_first_run_for_testing = first_run;
}

ArcPaths GetArcPaths() {
  if (PathsForTesting()) {
    return *PathsForTesting();
  }
#if BUILDFLAG(IS_MAC)
  base::FilePath app_support;
  base::FilePath caches;
  if (!base::PathService::Get(base::DIR_APP_DATA, &app_support)) {
    return ArcPaths();
  }
  base::PathService::Get(base::DIR_CACHE, &caches);
  return MacArcPaths(app_support, caches);
#elif BUILDFLAG(IS_WIN)
  base::FilePath local_app_data;
  if (!base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local_app_data)) {
    return ArcPaths();
  }
  return FindWindowsArcPaths(local_app_data).value_or(ArcPaths());
#else
  // Arc doesn't exist elsewhere: nothing to import.
  return ArcPaths();
#endif
}

ArcDecryptor GetArcDecryptor(const base::FilePath& user_data) {
#if BUILDFLAG(IS_WIN)
  std::optional<base::DictValue> local_state =
      ReadJsonDict(user_data.Append(chrome::kLocalStateFilename));
  std::optional<std::string> key =
      local_state ? browser_import::UnwrapWindowsKey(*local_state)
                  : std::nullopt;
  if (!key || key->size() != ChromiumWindowsCipher::kKeySize) {
    return ArcDecryptor();
  }
  return base::BindRepeating(
      [](const ChromiumWindowsCipher& cipher, std::string_view value) {
        return cipher.Decrypt(value);
      },
      ChromiumWindowsCipher(*key));
#else
  std::optional<std::string> password = ReadArcKeychainPassword();
  if (!password) {
    return ArcDecryptor();
  }
  return base::BindRepeating(
      [](const ChromiumMacCipher& cipher, std::string_view value) {
        return cipher.Decrypt(value);
      },
      ChromiumMacCipher(*password));
#endif
}

void SetArcPathsForTesting(std::optional<ArcPaths> paths) {
  PathsForTesting() = std::move(paths);
}

bool SignImportedPreferences(const base::FilePath& profile_dir,
                             const std::string& arc_dir) {
  const base::FilePath preferences =
      profile_dir.Append(chrome::kPreferencesFilename);
  const base::FilePath secure_preferences =
      profile_dir.Append(kSecurePreferencesFilename);
  base::DictValue merged =
      ReadJsonDict(preferences).value_or(base::DictValue());
  merged.Remove(kProtectionKey);
  if (std::optional<base::DictValue> secure = ReadJsonDict(secure_preferences)) {
    secure->Remove(kProtectionKey);
    merged.Merge(std::move(*secure));
  }
  merged.SetByDottedPath(prefs::kArcImportProfileDir, arc_dir);
  base::DeleteFile(secure_preferences);
  base::DeleteFile(preferences);
  // Signed without Compass's encryption service, as first run seeds initial
  // preferences: the encrypted signatures are added when the profile loads
  // (PrefHashFilter, "migrated" preferences).
  return chrome_prefs::InitializePrefsFromMasterPrefs(
      profile_dir, std::move(merged), /*os_crypt_async=*/nullptr);
}

void ImportArcProfile(const std::string& arc_dir,
                      const std::u16string& name,
                      base::OnceCallback<void(ProfileImport)> callback) {
  g_browser_process->os_crypt_async()->GetInstance(
      base::BindOnce(&StartCopy, arc_dir, name, std::move(callback)));
}

}  // namespace compass::arc
