// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/ui/arc_import/arc_importer.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_file_value_serializer.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/prefs/chrome_pref_service_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_paths_internal.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"

namespace compass::arc {

namespace {

constexpr char kSecurePreferencesFilename[] = "Secure Preferences";
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
                     Profile* profile) {
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

}  // namespace

ArcPaths GetArcPaths() {
  if (PathsForTesting()) {
    return *PathsForTesting();
  }
  ArcPaths paths;
  base::FilePath app_support;
  base::FilePath caches;
  if (base::PathService::Get(base::DIR_APP_DATA, &app_support)) {
    paths.user_data = app_support.Append("Arc").Append("User Data");
    paths.sidebar = app_support.Append("Arc").Append("StorableSidebar.json");
  }
  if (base::PathService::Get(base::DIR_CACHE, &caches)) {
    paths.cache = caches.Append("Arc").Append("User Data");
  }
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
