// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/browser/browser_import/chromium_crypto.h"

#include "base/files/scoped_temp_dir.h"
#include "build/build_config.h"
#include "compass/browser/arc_import/arc_crypto.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::browser_import {
namespace {

constexpr char kKey[] = "0123456789abcdef0123456789abcdef";  // 32 bytes.

TEST(ChromiumCryptoTest, WindowsSchemeRoundTrips) {
  const std::string value = EncryptWindowsValueForTesting(kKey, "secret");
  EXPECT_TRUE(value.starts_with("v10"));
  EXPECT_EQ(DecryptWindowsValue(kKey, value), "secret");
  EXPECT_FALSE(
      DecryptWindowsValue("fedcba9876543210fedcba9876543210", value));
  // App-bound values only the browser itself reads.
  std::string app_bound = value;
  app_bound[1] = '2';
  EXPECT_FALSE(DecryptWindowsValue(kKey, app_bound));
}

TEST(ChromiumCryptoTest, KeychainServices) {
  EXPECT_EQ(KeychainService(Browser::kChrome), "Chrome Safe Storage");
  EXPECT_EQ(KeychainService(Browser::kEdge), "Microsoft Edge Safe Storage");
  EXPECT_EQ(KeychainService(Browser::kBrave), "Brave Safe Storage");
}

#if BUILDFLAG(IS_WIN)
TEST(ChromiumCryptoTest, LocalStatesKeyUnwrapsWithDpapi) {
  base::DictValue local_state;
  local_state.SetByDottedPath("os_crypt.encrypted_key",
                              WrapWindowsKeyForTesting(kKey));
  EXPECT_EQ(UnwrapWindowsKey(local_state), kKey);
  EXPECT_FALSE(UnwrapWindowsKey(base::DictValue()));
}
#endif

#if BUILDFLAG(IS_LINUX)
TEST(ChromiumCryptoTest, LinuxV10UsesTheFixedKey) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  PasswordDecryptor decrypt =
      ChromiumPasswordDecryptor(Browser::kChrome, dir.GetPath());
  ASSERT_TRUE(decrypt);
  const std::string value =
      arc::ChromiumMacCipher("peanuts", 1).Encrypt("secret");
  EXPECT_EQ(decrypt.Run(value), "secret");
}
#endif

#if BUILDFLAG(IS_MAC)
TEST(ChromiumCryptoTest, MacUsesTheKeychainPassword) {
  SetChromiumKeyForTesting("keychain password");
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  PasswordDecryptor decrypt =
      ChromiumPasswordDecryptor(Browser::kChrome, dir.GetPath());
  SetChromiumKeyForTesting(std::nullopt);
  ASSERT_TRUE(decrypt);
  const std::string value =
      arc::ChromiumMacCipher("keychain password").Encrypt("secret");
  EXPECT_EQ(decrypt.Run(value), "secret");
}
#endif

}  // namespace
}  // namespace compass::browser_import
