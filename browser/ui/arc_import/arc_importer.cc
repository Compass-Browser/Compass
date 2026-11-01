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
#include "chrome/browser/prefs/chrome_pref_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_paths_internal.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "compass/browser/arc_import/sidebar_import.h"
#include "compass/browser/window_sync/space_directory.h"
#include "compass/browser/window_sync/space_service.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace compass::arc {

namespace {

constexpr base::FilePath::CharType kSecurePreferencesFilename[] =
    FILE_PATH_LITERAL("Secure Preferences");
// Arc's and Compass's signatures of protected preferences.
constexpr char kProtectionKey[] = "protection";
// Where an imported profile came from, for importing again.
constexpr char kImportedFromPath[] = "compass.arc_import.profile_dir";

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

// Copies the profile on a blocking thread: reads Arc's key, then copies and
// re-encrypts with Compass's |encryptor|.
CopyResult CopyOnPool(const std::string& arc_dir,
                      const base::FilePath& from,
                      const base::FilePath& to,
                      const base::FilePath& from_cache,
                      const base::FilePath& to_cache,
                      scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  std::optional<std::string> password = ReadArcKeychainPassword();
  if (!password) {
    LOG(ERROR) << "Arc import: no access to Arc's Keychain password";
    return CopyResult();
  }
  const ChromiumMacCipher arc_cipher(*password);
  CopyResult result = CopyProfile(
      from, to, from_cache, to_cache,
      base::BindRepeating(
          [](const ChromiumMacCipher& arc_cipher,
             const os_crypt_async::Encryptor* encryptor,
             std::string_view value) -> std::optional<std::string> {
            std::optional<std::string> plaintext = arc_cipher.Decrypt(value);
            std::string ciphertext;
            if (!plaintext ||
                !encryptor->EncryptString(*plaintext, &ciphertext)) {
              return std::nullopt;
            }
            return ciphertext;
          },
          arc_cipher, base::RetainedRef(encryptor)));
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
  const ArcPaths arc = GetArcPaths();
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
      base::BindOnce(&CopyOnPool, arc_dir, arc.user_data.AppendASCII(arc_dir),
                     to,
                     arc.cache.AppendASCII(arc_dir), to_cache,
                     std::move(encryptor)),
      base::BindOnce(&OnCopied, to, std::move(name),
                     std::move(callback)));
}

// Reads Arc's sidebar and its profiles' names. Blocking.
std::optional<Sidebar> ReadSidebar(const ArcPaths& paths) {
  std::optional<base::DictValue> sidebar = ReadJsonDict(paths.sidebar);
  if (!sidebar) {
    return std::nullopt;
  }
  std::optional<base::DictValue> local_state =
      ReadJsonDict(paths.user_data.Append(chrome::kLocalStateFilename));
  return ParseSidebar(*sidebar, local_state ? &*local_state : nullptr);
}

// One import from Arc: its profiles one after another. Owns itself until
// it reports.
class WholeImport {
 public:
  WholeImport(Sidebar sidebar,
              base::OnceCallback<void(ArcImportResult)> callback)
      : sidebar_(std::move(sidebar)), callback_(std::move(callback)) {
    result_.ok = true;
  }

  // Only these Arc profiles.
  void KeepOnly(const std::vector<std::string>& dirs) {
    if (dirs.empty()) {
      return;
    }
    std::erase_if(sidebar_.profiles, [&](const Profile& profile) {
      return !std::ranges::contains(dirs, profile.dir);
    });
  }

  void Next() {
    if (next_ >= sidebar_.profiles.size()) {
      std::move(callback_).Run(std::move(result_));
      delete this;
      return;
    }
    const Profile& arc_profile = sidebar_.profiles[next_];
    ImportArcProfile(arc_profile.dir, arc_profile.name,
                     base::BindOnce(&WholeImport::OnProfileImported,
                                    weak_factory_.GetWeakPtr()));
  }

 private:
  void OnProfileImported(ProfileImport import) {
    const Profile& arc_profile = sidebar_.profiles[next_];
    ::Profile* profile = import.profile;
    if (!profile) {
      result_.failed.push_back(arc_profile.dir);
      ++next_;
      Next();
      return;
    }
    result_.profiles.push_back(profile);
    SpaceService* service = SpaceService::Get(profile);
    service->RunWhenLoaded(base::BindOnce(&WholeImport::OnSpacesLoaded,
                                          weak_factory_.GetWeakPtr(),
                                          profile->GetWeakPtr()));
  }

  void OnSpacesLoaded(base::WeakPtr<::Profile> profile) {
    const Profile& arc_profile = sidebar_.profiles[next_];
    if (profile) {
      std::vector<Space> spaces;
      for (const Space& space : sidebar_.spaces) {
        if (space.profile_dir == arc_profile.dir) {
          spaces.push_back(space);
        }
      }
      std::vector<SpaceModel*> added =
          AddArcSidebar(SpaceService::Get(profile.get())->space_list(),
                        arc_profile, spaces, base::Time::Now());
      if (!added.empty() && !result_.first_space.is_valid()) {
        result_.first_space = added.front()->id();
      }
      // Its spaces show in every window's switcher.
      if (SpaceDirectory* directory = SpaceDirectory::Get()) {
        directory->List(profile.get());
      }
    }
    ++next_;
    Next();
  }

  Sidebar sidebar_;
  base::OnceCallback<void(ArcImportResult)> callback_;
  ArcImportResult result_;
  size_t next_ = 0;
  base::WeakPtrFactory<WholeImport> weak_factory_{this};
};

void OnSidebarRead(std::vector<std::string> dirs,
                   base::OnceCallback<void(ArcImportResult)> callback,
                   std::optional<Sidebar> sidebar) {
  if (!sidebar) {
    std::move(callback).Run(ArcImportResult());
    return;
  }
  auto* import = new WholeImport(std::move(*sidebar), std::move(callback));
  import->KeepOnly(dirs);
  import->Next();
}

// The Arc profile folders Compass's profiles under |user_data| came from.
// Blocking.
std::set<std::string> FindImported(const base::FilePath& user_data) {
  std::set<std::string> imported;
  base::FileEnumerator profiles(user_data, /*recursive=*/false,
                                base::FileEnumerator::DIRECTORIES);
  for (base::FilePath dir = profiles.Next(); !dir.empty();
       dir = profiles.Next()) {
    std::optional<base::DictValue> prefs =
        ReadJsonDict(dir.Append(chrome::kPreferencesFilename));
    const std::string* from =
        prefs ? prefs->FindStringByDottedPath(kImportedFromPath) : nullptr;
    if (from) {
      imported.insert(*from);
    }
  }
  return imported;
}

ArcOverview ReadOverview(const ArcPaths& paths,
                         const base::FilePath& user_data) {
  ArcOverview overview;
  overview.sidebar = ReadSidebar(paths);
  overview.imported = FindImported(user_data);
  return overview;
}

}  // namespace

ArcImportResult::ArcImportResult() = default;
ArcImportResult::ArcImportResult(ArcImportResult&&) = default;
ArcImportResult& ArcImportResult::operator=(ArcImportResult&&) = default;
ArcImportResult::~ArcImportResult() = default;

void ImportFromArc(std::vector<std::string> dirs,
                   base::OnceCallback<void(ArcImportResult)> callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadSidebar, GetArcPaths()),
      base::BindOnce(&OnSidebarRead, std::move(dirs), std::move(callback)));
}

ArcOverview::ArcOverview() = default;
ArcOverview::ArcOverview(ArcOverview&&) = default;
ArcOverview& ArcOverview::operator=(ArcOverview&&) = default;
ArcOverview::~ArcOverview() = default;

void GetArcOverview(base::OnceCallback<void(ArcOverview)> callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadOverview, GetArcPaths(),
                     g_browser_process->profile_manager()->user_data_dir()),
      std::move(callback));
}

ArcPaths GetArcPaths() {
  if (PathsForTesting()) {
    return *PathsForTesting();
  }
  ArcPaths paths;
#if BUILDFLAG(IS_MAC)
  base::FilePath app_support;
  base::FilePath caches;
  if (base::PathService::Get(base::DIR_APP_DATA, &app_support)) {
    paths.user_data = app_support.AppendASCII("Arc").AppendASCII("User Data");
    paths.sidebar =
        app_support.AppendASCII("Arc").AppendASCII("StorableSidebar.json");
  }
  if (base::PathService::Get(base::DIR_CACHE, &caches)) {
    paths.cache = caches.AppendASCII("Arc").AppendASCII("User Data");
  }
#endif
  // Elsewhere the paths stay empty: Arc's data isn't found, so there is
  // nothing to import (Arc for Windows keeps it inside its MSIX package).
  return paths;
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
  merged.SetByDottedPath(kImportedFromPath, arc_dir);
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
