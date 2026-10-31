// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/chain.h"

#include <string>
#include <vector>

#include "base/base64url.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"

namespace compass::sync {
namespace {

// The test vectors of docs/design/sync.md, "Derivations": seed 00 01 .. 0f.
Seed TestSeed() {
  Seed seed;
  for (size_t i = 0; i < seed.size(); ++i) {
    seed[i] = static_cast<uint8_t>(i);
  }
  return seed;
}

TEST(ChainTest, DeriveAuthKey) {
  EXPECT_EQ(base::HexEncodeLower(DeriveAuthKey(TestSeed())),
            "be6eabbf861e7b696f17ae6bd46b4bd745d1190e0658e720ca55fa65104139ad");
  // Slice 5's per-profile accounts.
  EXPECT_EQ(base::HexEncodeLower(DeriveAuthKey(
                TestSeed(), "2b5c3e1a-0000-4000-8000-000000000001")),
            "1b28e57a6e3ca95bc1b4b9c30f10f8d554ef09eed00844b52048aea6d90f4cf4");
}

TEST(ChainTest, PublicKeyAndChainId) {
  const AuthKey key = DeriveAuthKey(TestSeed());
  EXPECT_EQ(base::HexEncodeLower(GetPublicKey(key)),
            "696a94d1a2ebbabf8e5012c07df3c65c7c6beaaf8ccaf4a67e6cdd3da9743467");
  EXPECT_EQ(GetChainId(key),
            "696a94d1a2ebbabf8e5012c07df3c65c7c6beaaf8ccaf4a67e6cdd3da9743467");
}

TEST(ChainTest, DerivePassphrase) {
  EXPECT_EQ(DerivePassphrase(TestSeed()),
            "xoFg1Bm10gjM8Ahji5ibCeSkV6zo7kZEcjBFP9M8s50=");
}

TEST(ChainTest, AccessTokenVector) {
  // Ed25519 signatures are deterministic, so the whole token is fixed.
  EXPECT_EQ(
      MakeAccessToken(
          DeriveAuthKey(TestSeed()),
          base::Time::FromMillisecondsSinceUnixEpoch(1700000000000)),
      "MzEzNzMwMzAzMDMwMzAzMDMwMzAzMDMwMzB8MkY1REY4REVCNDUwRUJBMTk5MTgxNjdGOD"
      "ZFQjVDOThBRkQxREYyRUI5MTUzQjBDOENGMDQ1RDhGQjZDODk3MERFMjhFOUI0Qzg0QzJC"
      "MjUzRDcxMzI4QTVCNUJENzcwNUE0QTQ1QjFCQkVERkJBRjM5MzcyQTA1RTZEOTZCMDV8Nj"
      "k2QTk0RDFBMkVCQkFCRjhFNTAxMkMwN0RGM0M2NUM3QzZCRUFBRjhDQ0FGNEE2N0U2Q0RE"
      "M0RBOTc0MzQ2Nw==");
}

TEST(ChainTest, AccessTokenFormat) {
  const AuthKey key = DeriveAuthKey(TestSeed());
  const base::Time now =
      base::Time::FromMillisecondsSinceUnixEpoch(1761900000123);
  std::string plain;
  ASSERT_TRUE(base::Base64UrlDecode(
      MakeAccessToken(key, now), base::Base64UrlDecodePolicy::REQUIRE_PADDING,
      &plain));
  const std::vector<std::string> parts = base::SplitString(
      plain, "|", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  ASSERT_EQ(parts.size(), 3u);

  std::string timestamp;
  ASSERT_TRUE(base::HexStringToString(parts[0], &timestamp));
  EXPECT_EQ(timestamp, "1761900000123");
  std::vector<uint8_t> signature;
  ASSERT_TRUE(base::HexStringToBytes(parts[1], &signature));
  ASSERT_EQ(signature.size(), 64u);
  EXPECT_EQ(base::ToLowerASCII(parts[2]), GetChainId(key));
  // Uppercase hex, as go-sync's client sends it.
  EXPECT_EQ(parts[2], base::ToUpperASCII(parts[2]));

  const PublicKey public_key = GetPublicKey(key);
  EXPECT_TRUE(ED25519_verify(reinterpret_cast<const uint8_t*>(timestamp.data()),
                             timestamp.size(), signature.data(),
                             public_key.data()));
}

TEST(ChainTest, RecoveryCodeRoundTrip) {
  EXPECT_EQ(base::JoinString(SeedToRecoveryWords(TestSeed()), " "),
            "abandon amount liar amount expire adjust cage candy arch gather "
            "drum buyer");
  const Seed seed = GenerateSeed();
  EXPECT_EQ(
      RecoveryCodeToSeed(base::JoinString(SeedToRecoveryWords(seed), " ")),
      seed);
}

TEST(ChainTest, GenerateSeedIsRandom) {
  EXPECT_NE(GenerateSeed(), GenerateSeed());
}

}  // namespace
}  // namespace compass::sync
