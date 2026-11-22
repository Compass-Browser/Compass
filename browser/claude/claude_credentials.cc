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
#include "compass/browser/claude/chatgpt_account.h"
#include "compass/browser/claude/claude_api.h"
#include "compass/browser/claude/claude_prefs.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
#include "components/prefs/pref_service.h"

namespace compass::claude {

namespace {

void StoreEncrypted(base::WeakPtr<Profile> profile,
                    AIProvider provider,
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
  prefs->SetString(prefs::ProviderPref(prefs::kApiKey, provider),
                   base::Base64Encode(ciphertext));
  prefs->SetString(prefs::ProviderPref(prefs::kApiKeyHint, provider),
                   KeyHint(key));
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
  const PrefService& prefs = *profile->GetPrefs();
  if (!base::Base64Decode(
          prefs.GetString(prefs::CurrentPref(prefs, prefs::kApiKey)),
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
  PrefService* prefs = profile->GetPrefs();
  // The provider in use now, even if it changes before the key is stored.
  const AIProvider provider = prefs::CurrentProvider(*prefs);
  if (trimmed.empty()) {
    prefs->ClearPref(prefs::ProviderPref(prefs::kApiKey, provider));
    prefs->ClearPref(prefs::ProviderPref(prefs::kApiKeyHint, provider));
    std::move(callback).Run(true);
    return;
  }
  g_browser_process->os_crypt_async()->GetInstance(
      base::BindOnce(&StoreEncrypted, profile->GetWeakPtr(), provider,
                     std::move(trimmed), std::move(callback)));
}

// static
void ClaudeCredentials::GetApiKey(
    Profile* profile,
    base::OnceCallback<void(std::optional<std::string>)> callback) {
  if (UsesChatGPT(*profile->GetPrefs())) {
    ChatGPTAccount::ForProfile(profile)->GetAccessToken(base::BindOnce(
        [](base::OnceCallback<void(std::optional<std::string>)> callback,
           base::expected<std::string, std::string> token) {
          std::move(callback).Run(token.has_value()
                                      ? std::optional<std::string>(*token)
                                      : std::nullopt);
        },
        std::move(callback)));
    return;
  }
  if (!HasApiKey(profile)) {
    // An OpenAI-compatible server may take no key.
    std::move(callback).Run(
        IsSetUp(profile) ? std::optional<std::string>(std::string())
                         : std::nullopt);
    return;
  }
  g_browser_process->os_crypt_async()->GetInstance(base::BindOnce(
      &ReadDecrypted, profile->GetWeakPtr(), std::move(callback)));
}

// static
bool ClaudeCredentials::HasApiKey(Profile* profile) {
  const PrefService& prefs = *profile->GetPrefs();
  return !prefs.GetString(prefs::CurrentPref(prefs, prefs::kApiKey)).empty();
}

// static
bool ClaudeCredentials::IsSetUp(Profile* profile) {
  return IsSetUp(*profile->GetPrefs());
}

// static
bool ClaudeCredentials::IsSetUp(const PrefService& prefs) {
  if (UsesChatGPT(prefs)) {
    return true;
  }
  const AIProvider provider = prefs::CurrentProvider(prefs);
  if (!prefs.GetString(prefs::ProviderPref(prefs::kApiKey, provider))
           .empty()) {
    return true;
  }
  return provider == AIProvider::kCompatible &&
         ParseBaseUrl(prefs.GetString(
                          prefs::ProviderPref(prefs::kBaseUrl, provider)),
                      provider)
             .has_value();
}

// static
std::string ClaudeCredentials::GetApiKeyHint(Profile* profile) {
  const PrefService& prefs = *profile->GetPrefs();
  return HasApiKey(profile)
             ? prefs.GetString(prefs::CurrentPref(prefs, prefs::kApiKeyHint))
             : std::string();
}

// static
std::string ClaudeCredentials::MissingKeyMessage(Profile* profile) {
  const PrefService& prefs = *profile->GetPrefs();
  if (prefs::CurrentProvider(prefs) == AIProvider::kOpenAI) {
    ChatGPTAccount* account = ChatGPTAccount::ForProfile(profile);
    if (UsesChatGPT(prefs) && !account->last_token_error().empty()) {
      return account->last_token_error();
    }
    if (!account->ended_reason().empty() && !HasApiKey(profile)) {
      return account->ended_reason();
    }
  }
  if (HasApiKey(profile)) {
    return "The saved key can't be read any more. Enter it again in "
           "Compass's AI settings.";
  }
  return "Add your API key in Compass's AI settings.";
}

}  // namespace compass::claude
