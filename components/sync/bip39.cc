// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/bip39.h"

#include <algorithm>

#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "crypto/hash.h"

namespace compass::sync {
namespace {

constexpr size_t kBitsPerWord = 11;
constexpr size_t kEntropyBits = kRecoveryEntropySize * 8;  // 128
constexpr size_t kChecksumBits = kEntropyBits / 32;        // 4
static_assert((kEntropyBits + kChecksumBits) ==
              kRecoveryCodeWords * kBitsPerWord);

// The first kChecksumBits bits of SHA-256(entropy), in the high bits.
uint8_t Checksum(base::span<const uint8_t, kRecoveryEntropySize> entropy) {
  const auto digest = crypto::hash::Sha256(entropy);
  return digest[0] & static_cast<uint8_t>(0xff << (8 - kChecksumBits));
}

}  // namespace

std::optional<int> Bip39WordIndex(std::string_view word) {
  const auto words = Bip39EnglishWords();
  const auto it = std::lower_bound(words.begin(), words.end(), word);
  if (it == words.end() || *it != word) {
    return std::nullopt;
  }
  return static_cast<int>(it - words.begin());
}

std::vector<std::string> EntropyToRecoveryWords(
    base::span<const uint8_t, kRecoveryEntropySize> entropy) {
  const uint8_t checksum = Checksum(entropy);
  // Bit |i| of entropy || checksum, most significant first.
  auto bit = [&](size_t i) -> int {
    const uint8_t byte = i < kEntropyBits ? entropy[i / 8] : checksum;
    return (byte >> (7 - i % 8)) & 1;
  };
  const auto words = Bip39EnglishWords();
  std::vector<std::string> result;
  result.reserve(kRecoveryCodeWords);
  for (size_t w = 0; w < kRecoveryCodeWords; ++w) {
    int index = 0;
    for (size_t b = 0; b < kBitsPerWord; ++b) {
      index = (index << 1) | bit(w * kBitsPerWord + b);
    }
    result.emplace_back(words[static_cast<size_t>(index)]);
  }
  return result;
}

std::optional<std::array<uint8_t, kRecoveryEntropySize>> RecoveryWordsToEntropy(
    const std::vector<std::string>& words) {
  if (words.size() != kRecoveryCodeWords) {
    return std::nullopt;
  }
  std::array<uint8_t, kRecoveryEntropySize> entropy{};
  uint8_t checksum = 0;
  for (size_t w = 0; w < kRecoveryCodeWords; ++w) {
    const std::optional<int> index = Bip39WordIndex(words[w]);
    if (!index) {
      return std::nullopt;
    }
    for (size_t b = 0; b < kBitsPerWord; ++b) {
      const size_t i = w * kBitsPerWord + b;
      const int value = (*index >> (kBitsPerWord - 1 - b)) & 1;
      if (!value) {
        continue;
      }
      if (i < kEntropyBits) {
        entropy[i / 8] |= static_cast<uint8_t>(1 << (7 - i % 8));
      } else {
        checksum |= static_cast<uint8_t>(1 << (7 - (i - kEntropyBits)));
      }
    }
  }
  if (Checksum(entropy) != checksum) {
    return std::nullopt;
  }
  return entropy;
}

std::optional<std::array<uint8_t, kRecoveryEntropySize>> ParseRecoveryCode(
    std::string_view text) {
  std::vector<std::string> words;
  for (std::string_view word : base::SplitStringPiece(
           text, base::kWhitespaceASCII, base::TRIM_WHITESPACE,
           base::SPLIT_WANT_NONEMPTY)) {
    words.push_back(base::ToLowerASCII(word));
  }
  return RecoveryWordsToEntropy(words);
}

std::vector<std::string_view> Bip39WordsWithPrefix(std::string_view prefix,
                                                   size_t max) {
  std::vector<std::string_view> result;
  const auto words = Bip39EnglishWords();
  for (auto it = std::lower_bound(words.begin(), words.end(), prefix);
       it != words.end() && result.size() < max && it->starts_with(prefix);
       ++it) {
    result.push_back(*it);
  }
  return result;
}

}  // namespace compass::sync
