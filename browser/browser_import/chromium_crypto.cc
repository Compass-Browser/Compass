// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/chromium_crypto.h"

#include <array>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/no_destructor.h"
#include "build/build_config.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "crypto/aead.h"
#include "crypto/random.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <dpapi.h>
#endif

namespace compass::browser_import {

namespace {

constexpr std::string_view kV10 = "v10";
constexpr size_t kGcmNonceSize = 12;

std::optional<std::string>& KeyForTesting() {
  static base::NoDestructor<std::optional<std::string>> key;
  return *key;
}

[[maybe_unused]] std::optional<std::string> DecryptCbc(const arc::ChromiumMacCipher& cipher,
                                      std::string_view value) {
  return cipher.Decrypt(value);
}

[[maybe_unused]] std::optional<base::DictValue> ReadLocalState(
    const base::FilePath& user_data) {
  std::string json;
  if (!base::ReadFileToString(user_data.AppendASCII("Local State"), &json)) {
    return std::nullopt;
  }
  std::optional<base::Value> value = base::JSONReader::Read(
      json, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!value || !value->is_dict()) {
    return std::nullopt;
  }
  return std::move(*value).TakeDict();
}

}  // namespace

std::string_view KeychainService(Browser browser) {
  switch (browser) {
    case Browser::kChrome:
      return "Chrome Safe Storage";
    case Browser::kEdge:
      return "Microsoft Edge Safe Storage";
    case Browser::kBrave:
      return "Brave Safe Storage";
    case Browser::kVivaldi:
      return "Vivaldi Safe Storage";
    case Browser::kFirefox:
    case Browser::kZen:
    case Browser::kSafari:
      return "";
  }
}

void SetChromiumKeyForTesting(std::optional<std::string> key) {
  KeyForTesting() = std::move(key);
}

std::optional<std::string> DecryptWindowsValue(std::string_view key,
                                               std::string_view value) {
  if (!value.starts_with(kV10) ||
      value.size() < kV10.size() + kGcmNonceSize) {
    return std::nullopt;
  }
  value.remove_prefix(kV10.size());
  const std::string_view nonce = value.substr(0, kGcmNonceSize);
  const std::string_view ciphertext = value.substr(kGcmNonceSize);
  std::optional<std::vector<uint8_t>> plaintext = crypto::aead::Open(
      crypto::aead::AES_256_GCM, base::as_byte_span(key),
      base::as_byte_span(ciphertext), base::as_byte_span(nonce), {});
  if (!plaintext) {
    return std::nullopt;
  }
  return std::string(plaintext->begin(), plaintext->end());
}

std::string EncryptWindowsValueForTesting(std::string_view key,
                                          std::string_view plaintext) {
  std::array<uint8_t, kGcmNonceSize> nonce;
  crypto::RandBytes(nonce);
  std::vector<uint8_t> sealed = crypto::aead::Seal(
      crypto::aead::AES_256_GCM, base::as_byte_span(key),
      base::as_byte_span(plaintext), nonce, {});
  return std::string(kV10) + std::string(nonce.begin(), nonce.end()) +
         std::string(sealed.begin(), sealed.end());
}

#if BUILDFLAG(IS_WIN)
namespace {
constexpr std::string_view kDpapiPrefix = "DPAPI";
}  // namespace

std::optional<std::string> UnwrapWindowsKey(
    const base::DictValue& local_state) {
  const std::string* encoded =
      local_state.FindStringByDottedPath("os_crypt.encrypted_key");
  std::string wrapped;
  if (!encoded || !base::Base64Decode(*encoded, &wrapped) ||
      !wrapped.starts_with(kDpapiPrefix)) {
    return std::nullopt;
  }
  wrapped.erase(0, kDpapiPrefix.size());
  DATA_BLOB input = {static_cast<DWORD>(wrapped.size()),
                     reinterpret_cast<BYTE*>(wrapped.data())};
  DATA_BLOB output = {};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return std::nullopt;
  }
  std::string key(reinterpret_cast<const char*>(output.pbData),
                  output.cbData);
  LocalFree(output.pbData);
  return key;
}

std::string WrapWindowsKeyForTesting(std::string_view key) {
  DATA_BLOB input = {static_cast<DWORD>(key.size()),
                     reinterpret_cast<BYTE*>(const_cast<char*>(key.data()))};
  DATA_BLOB output = {};
  if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return std::string();
  }
  std::string wrapped(kDpapiPrefix);
  wrapped.append(reinterpret_cast<const char*>(output.pbData), output.cbData);
  LocalFree(output.pbData);
  return base::Base64Encode(wrapped);
}
#endif  // BUILDFLAG(IS_WIN)

PasswordDecryptor ChromiumPasswordDecryptor(Browser browser,
                                            const base::FilePath& user_data) {
#if BUILDFLAG(IS_MAC)
  std::optional<std::string> password = KeyForTesting();
  if (!password) {
    password = ReadKeychainPassword(KeychainService(browser));
  }
  if (!password) {
    return PasswordDecryptor();
  }
  return base::BindRepeating(&DecryptCbc, arc::ChromiumMacCipher(*password));
#elif BUILDFLAG(IS_WIN)
  std::optional<std::string> key = KeyForTesting();
  if (!key) {
    std::optional<base::DictValue> local_state = ReadLocalState(user_data);
    if (local_state) {
      key = UnwrapWindowsKey(*local_state);
    }
  }
  if (!key) {
    return PasswordDecryptor();
  }
  return base::BindRepeating(
      [](const std::string& key, std::string_view value) {
        return DecryptWindowsValue(key, value);
      },
      *key);
#elif BUILDFLAG(IS_LINUX)
  // Only "v10" (no keyring): Chromium's fixed password, one round.
  return base::BindRepeating(&DecryptCbc,
                             arc::ChromiumMacCipher("peanuts", 1));
#else
  return PasswordDecryptor();
#endif
}

}  // namespace compass::browser_import
