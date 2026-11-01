// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/os_crypt/legacy_keychain_password.h"

#include <Security/Security.h>

#include <map>
#include <string>
#include <utility>

#include "base/containers/to_vector.h"
#include "base/types/expected.h"
#include "components/os_crypt/common/keychain_password_mac.h"
#include "crypto/apple/fake_keychain_v2.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass {
namespace {

// A keychain of generic passwords keyed by (service, account), with an
// error a lookup of one service can be made to return.
class GenericPasswordKeychain : public crypto::apple::FakeKeychainV2 {
 public:
  GenericPasswordKeychain() : FakeKeychainV2("test") {}

  OSStatus AddGenericPassword(std::string_view service_name,
                              std::string_view account_name,
                              base::span<const uint8_t> password) override {
    ++adds_;
    if (add_error_ != noErr) {
      return add_error_;
    }
    items_[{std::string(service_name), std::string(account_name)}] =
        std::string(password.begin(), password.end());
    return noErr;
  }

  base::expected<std::vector<uint8_t>, OSStatus> FindGenericPassword(
      std::string_view service_name,
      std::string_view account_name) override {
    if (service_name == failing_service_) {
      return base::unexpected(find_error_);
    }
    auto it =
        items_.find({std::string(service_name), std::string(account_name)});
    if (it == items_.end()) {
      return base::unexpected(errSecItemNotFound);
    }
    return base::ToVector(it->second, [](char c) { return uint8_t(c); });
  }

  void Put(const std::string& service, const std::string& account,
           const std::string& password) {
    items_[{service, account}] = password;
  }
  std::optional<std::string> Get(const std::string& service,
                                 const std::string& account) const {
    auto it = items_.find({service, account});
    return it == items_.end() ? std::nullopt
                              : std::optional<std::string>(it->second);
  }
  void FailFindsOf(const std::string& service, OSStatus error) {
    failing_service_ = service;
    find_error_ = error;
  }
  void FailAdds(OSStatus error) { add_error_ = error; }
  int adds() const { return adds_; }

 private:
  std::map<std::pair<std::string, std::string>, std::string> items_;
  std::string failing_service_;
  OSStatus find_error_ = noErr;
  OSStatus add_error_ = noErr;
  int adds_ = 0;
};

TEST(CompassSafeStorageTest, UsesCompassItem) {
  EXPECT_EQ(kSafeStorageServiceName, KeychainPassword::GetServiceName());
  EXPECT_EQ(kSafeStorageAccountName, KeychainPassword::GetAccountName());
}

TEST(CompassSafeStorageTest, ExistingItemIsUsed) {
  GenericPasswordKeychain keychain;
  keychain.Put(kSafeStorageServiceName, kSafeStorageAccountName, "new-key");
  keychain.Put(kLegacySafeStorageServiceName, kLegacySafeStorageAccountName,
               "old-key");
  EXPECT_EQ("new-key", KeychainPassword(keychain).GetPassword());
  EXPECT_EQ(0, keychain.adds());
}

TEST(CompassSafeStorageTest, LegacyKeyMoves) {
  GenericPasswordKeychain keychain;
  keychain.Put(kLegacySafeStorageServiceName, kLegacySafeStorageAccountName,
               "old-key");
  EXPECT_EQ("old-key", KeychainPassword(keychain).GetPassword());
  EXPECT_EQ("old-key",
            keychain.Get(kSafeStorageServiceName, kSafeStorageAccountName));
  // The legacy item stays for older builds.
  EXPECT_EQ("old-key", keychain.Get(kLegacySafeStorageServiceName,
                                    kLegacySafeStorageAccountName));
  // The next launch finds the new item.
  EXPECT_EQ("old-key", KeychainPassword(keychain).GetPassword());
  EXPECT_EQ(1, keychain.adds());
}

TEST(CompassSafeStorageTest, NoLegacyKeyMakesANewOne) {
  GenericPasswordKeychain keychain;
  const std::string key = KeychainPassword(keychain).GetPassword();
  EXPECT_EQ(24u, key.size());
  EXPECT_EQ(key,
            keychain.Get(kSafeStorageServiceName, kSafeStorageAccountName));
  EXPECT_FALSE(keychain.Get(kLegacySafeStorageServiceName,
                            kLegacySafeStorageAccountName));
}

TEST(CompassSafeStorageTest, UnreadableLegacyKeyIsNotReplaced) {
  GenericPasswordKeychain keychain;
  keychain.FailFindsOf(kLegacySafeStorageServiceName, errSecAuthFailed);
  EXPECT_EQ("", KeychainPassword(keychain).GetPassword());
  EXPECT_EQ(0, keychain.adds());
}

TEST(CompassSafeStorageTest, FailedCopyStillUsesTheLegacyKey) {
  GenericPasswordKeychain keychain;
  keychain.Put(kLegacySafeStorageServiceName, kLegacySafeStorageAccountName,
               "old-key");
  keychain.FailAdds(errSecDuplicateItem);
  EXPECT_EQ("old-key", KeychainPassword(keychain).GetPassword());
  EXPECT_FALSE(keychain.Get(kSafeStorageServiceName, kSafeStorageAccountName));
}

TEST(CompassSafeStorageTest, OtherItemsDontMove) {
  GenericPasswordKeychain keychain;
  keychain.Put(kLegacySafeStorageServiceName, kLegacySafeStorageAccountName,
               "old-key");
  EXPECT_EQ(std::nullopt,
            MoveLegacyKeychainPassword(keychain, "Other Safe Storage",
                                       "Other"));
  EXPECT_EQ(0, keychain.adds());
}

}  // namespace
}  // namespace compass
