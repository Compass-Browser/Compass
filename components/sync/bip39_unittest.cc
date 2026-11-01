// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/bip39.h"

#include <string>
#include <vector>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::sync {
namespace {

using ::testing::ElementsAre;

struct Vector {
  const char* entropy_hex;
  const char* words;
};

// The 128-bit English vectors of the BIP39 reference implementation
// (trezor/python-mnemonic vectors.json), plus our derivation test seed.
constexpr Vector kVectors[] = {
    {"00000000000000000000000000000000",
     "abandon abandon abandon abandon abandon abandon abandon abandon abandon "
     "abandon abandon about"},
    {"7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
     "legal winner thank year wave sausage worth useful legal winner thank "
     "yellow"},
    {"80808080808080808080808080808080",
     "letter advice cage absurd amount doctor acoustic avoid letter advice "
     "cage above"},
    {"ffffffffffffffffffffffffffffffff",
     "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"},
    {"9e885d952ad362caeb4efe34a8e91bd2",
     "ozone drill grab fiber curtain grace pudding thank cruise elder eight "
     "picnic"},
    {"c0ba5a8e914111210f2bd131f3d5e08d",
     "scheme spot photo card baby mountain device kick cradle pact join "
     "borrow"},
    {"000102030405060708090a0b0c0d0e0f",
     "abandon amount liar amount expire adjust cage candy arch gather drum "
     "buyer"},
};

std::array<uint8_t, kRecoveryEntropySize> FromHex(const char* hex) {
  std::array<uint8_t, kRecoveryEntropySize> bytes{};
  EXPECT_TRUE(base::HexStringToSpan(hex, bytes));
  return bytes;
}

std::vector<std::string> Split(const char* words) {
  return base::SplitString(words, " ", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY);
}

TEST(Bip39Test, Wordlist) {
  const auto words = Bip39EnglishWords();
  EXPECT_EQ(words.front(), "abandon");
  EXPECT_EQ(words.back(), "zoo");
  for (size_t i = 1; i < words.size(); ++i) {
    EXPECT_LT(words[i - 1], words[i]) << i;
  }
  EXPECT_EQ(Bip39WordIndex("abandon"), 0);
  EXPECT_EQ(Bip39WordIndex("about"), 3);
  EXPECT_EQ(Bip39WordIndex("zoo"), 2047);
  EXPECT_EQ(Bip39WordIndex("compass"), std::nullopt);
  EXPECT_EQ(Bip39WordIndex(""), std::nullopt);
}

TEST(Bip39Test, EntropyToWords) {
  for (const Vector& vector : kVectors) {
    EXPECT_EQ(base::JoinString(
                  EntropyToRecoveryWords(FromHex(vector.entropy_hex)), " "),
              vector.words)
        << vector.entropy_hex;
  }
}

TEST(Bip39Test, WordsToEntropy) {
  for (const Vector& vector : kVectors) {
    std::optional<std::array<uint8_t, kRecoveryEntropySize>> entropy =
        RecoveryWordsToEntropy(Split(vector.words));
    ASSERT_TRUE(entropy) << vector.words;
    EXPECT_EQ(*entropy, FromHex(vector.entropy_hex)) << vector.words;
  }
}

TEST(Bip39Test, ChecksumIsChecked) {
  // "about" carries the checksum of zero entropy; "abandon" doesn't.
  EXPECT_FALSE(RecoveryWordsToEntropy(
      Split("abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon")));
  // All-ones entropy ends in "wrong" (checksum 0101), not "zoo" (1111).
  EXPECT_FALSE(RecoveryWordsToEntropy(
      Split("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo")));
}

TEST(Bip39Test, RejectsWrongLengthAndUnknownWords) {
  EXPECT_FALSE(RecoveryWordsToEntropy(
      Split("abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon about")));
  EXPECT_FALSE(RecoveryWordsToEntropy(
      Split("abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon about")));
  EXPECT_FALSE(RecoveryWordsToEntropy(
      Split("abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon compass")));
}

TEST(Bip39Test, ParseRecoveryCodeAcceptsTyping) {
  std::optional<std::array<uint8_t, kRecoveryEntropySize>> entropy =
      ParseRecoveryCode(
          "  Legal WINNER thank year\twave sausage\n worth useful legal "
          "winner thank yellow ");
  ASSERT_TRUE(entropy);
  EXPECT_EQ(*entropy, FromHex("7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f"));
  EXPECT_FALSE(ParseRecoveryCode(""));
}

TEST(Bip39Test, WordsWithPrefix) {
  EXPECT_THAT(Bip39WordsWithPrefix("abs", 10),
              ElementsAre("absent", "absorb", "abstract", "absurd"));
  EXPECT_THAT(Bip39WordsWithPrefix("abs", 2), ElementsAre("absent", "absorb"));
  EXPECT_THAT(Bip39WordsWithPrefix("zoo", 10), ElementsAre("zoo"));
  EXPECT_TRUE(Bip39WordsWithPrefix("qq", 10).empty());
}

}  // namespace
}  // namespace compass::sync
