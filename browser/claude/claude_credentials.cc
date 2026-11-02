// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/claude/claude_credentials.h"

#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_util.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_prefs.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"

namespace compass::claude {

namespace {

void StoreEncrypted(base::WeakPtr<Profile> profile,
                    std::string key,
                    base::OnceCallback<void(bool)> callback,
                    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  if (!profile) {
    std::move(callback).Run(false);
    return;
  }
  std::string ciphertext;
  if (!encryptor || !encryptor->EncryptString(key, &ciphertext)) {
    std::move(callback).Run(false);
    return;
  }
  PrefService* prefs = profile->GetPrefs();
  prefs->SetString(prefs::kApiKey, base::Base64Encode(ciphertext));
  prefs->SetString(prefs::kApiKeyHint, KeyHint(key));
  std::move(callback).Run(true);
}

void ReadDecrypted(
    base::WeakPtr<Profile> profile,
    base::OnceCallback<void(std::optional<std::string>)> callback,
    scoped_refptr<os_crypt_async::Encryptor> encryptor) {
  if (!profile || !encryptor) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  std::string ciphertext;
  if (!base::Base64Decode(profile->GetPrefs()->GetString(prefs::kApiKey),
                          &ciphertext) ||
      ciphertext.empty()) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  std::string key;
  if (!encryptor->DecryptString(ciphertext, &key) || key.empty()) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  std::move(callback).Run(std::move(key));
}

}  // namespace

// static
void ClaudeCredentials::SetApiKey(Profile* profile,
                                  const std::string& key,
                                  base::OnceCallback<void(bool)> callback) {
  std::string trimmed(base::TrimWhitespaceASCII(key, base::TRIM_ALL));
  if (trimmed.empty()) {
    PrefService* prefs = profile->GetPrefs();
    prefs->ClearPref(prefs::kApiKey);
    prefs->ClearPref(prefs::kApiKeyHint);
    std::move(callback).Run(true);
    return;
  }
  g_browser_process->os_crypt_async()->GetInstance(
      base::BindOnce(&StoreEncrypted, profile->GetWeakPtr(), std::move(trimmed),
                     std::move(callback)));
}

// static
void ClaudeCredentials::GetApiKey(
    Profile* profile,
    base::OnceCallback<void(std::optional<std::string>)> callback) {
  if (!HasApiKey(profile)) {
    std::move(callback).Run(std::nullopt);
    return;
  }
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      &ReadDecrypted, profile->GetWeakPtr(), std::move(callback)));
}

// static
bool ClaudeCredentials::HasApiKey(Profile* profile) {
  return !profile->GetPrefs()->GetString(prefs::kApiKey).empty();
}

// static
std::string ClaudeCredentials::GetApiKeyHint(Profile* profile) {
  return HasApiKey(profile)
             ? profile->GetPrefs()->GetString(prefs::kApiKeyHint)
             : std::string();
}

}  // namespace compass::claude
