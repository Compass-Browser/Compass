// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_BIP39_H_
#define COMPASS_COMPONENTS_SYNC_BIP39_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"

// The recovery code: the chain's 16-byte seed as 12 BIP39 English words
// (128 bits of entropy plus a 4-bit SHA-256 checksum). See
// docs/design/sync.md, "Identity".
namespace compass::sync {

inline constexpr size_t kBip39WordCount = 2048;
inline constexpr size_t kRecoveryEntropySize = 16;
inline constexpr size_t kRecoveryCodeWords = 12;

// The English wordlist, sorted (bip39_wordlist.cc).
base::span<const std::string_view, kBip39WordCount> Bip39EnglishWords();

// The index of |word| in the wordlist, or nullopt.
std::optional<int> Bip39WordIndex(std::string_view word);

// The 12 words for |entropy|.
std::vector<std::string> EntropyToRecoveryWords(
    base::span<const uint8_t, kRecoveryEntropySize> entropy);

// The entropy for 12 words, or nullopt if there aren't 12, a word isn't in
// the list, or the checksum doesn't match.
std::optional<std::array<uint8_t, kRecoveryEntropySize>> RecoveryWordsToEntropy(
    const std::vector<std::string>& words);

// Same, for text as the user types it: words separated by any whitespace,
// any case.
std::optional<std::array<uint8_t, kRecoveryEntropySize>> ParseRecoveryCode(
    std::string_view text);

// Up to |max| words that start with |prefix|, in list order (autocomplete).
std::vector<std::string_view> Bip39WordsWithPrefix(std::string_view prefix,
                                                   size_t max);

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_BIP39_H_
