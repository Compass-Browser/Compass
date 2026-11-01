// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_CHAIN_H_
#define COMPASS_COMPONENTS_SYNC_CHAIN_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "base/time/time.h"
#include "compass/components/sync/bip39.h"

// The sync chain: a random seed every synced device holds, and what is
// derived from it. See docs/design/sync.md, "Identity", for the derivations
// and test vectors.
namespace compass::sync {

inline constexpr size_t kSeedSize = kRecoveryEntropySize;  // 16
using Seed = std::array<uint8_t, kSeedSize>;

// An Ed25519 private key seed (RFC 8032), the input of
// ED25519_keypair_from_seed().
inline constexpr size_t kAuthKeySize = 32;
using AuthKey = std::array<uint8_t, kAuthKeySize>;
using PublicKey = std::array<uint8_t, 32>;

// HKDF info strings (salt empty, SHA-256, 32 bytes out).
inline constexpr char kAuthInfo[] = "compass-sync-auth";
inline constexpr char kPassphraseInfo[] = "compass-sync-passphrase";

// A new random seed.
Seed GenerateSeed();

// HKDF-SHA256(seed, salt="", info="compass-sync-auth"[":" + profile_uuid]).
// An empty |profile_uuid| gives the chain's own key (slices 1-4); profile
// accounts (slice 5) pass the profile's sync UUID.
AuthKey DeriveAuthKey(const Seed& seed, std::string_view profile_uuid = {});

// HKDF-SHA256(seed, salt="", info="compass-sync-passphrase"[":" +
// profile_uuid]), base64 (standard alphabet, padded): Chromium's custom sync
// passphrase. As for DeriveAuthKey(), an empty |profile_uuid| gives the
// chain's own (the home profile's); a profile account passes its sync UUID,
// so each account has its own Nigori keys (docs/design/sync.md, "Slice 5
// notes").
std::string DerivePassphrase(const Seed& seed,
                             std::string_view profile_uuid = {});

PublicKey GetPublicKey(const AuthKey& key);

// The chain's ID on the server: the public key, lowercase hex.
std::string GetChainId(const AuthKey& key);

// go-sync's access token (auth/auth.go in brave/go-sync), sent as
// "Authorization: Bearer <token>":
//   base64url_padded(HEX(ts) "|" HEX(ed25519_sign(ts)) "|" HEX(pubkey))
// where ts is the Unix time in milliseconds as ASCII decimal and HEX is
// uppercase. The server accepts it for +/- 24 h around its clock.
std::string MakeAccessToken(const AuthKey& key, base::Time now);

// The recovery code for |seed|: 12 BIP39 words.
std::vector<std::string> SeedToRecoveryWords(const Seed& seed);

// The seed for a recovery code as typed, or nullopt (see ParseRecoveryCode()).
std::optional<Seed> RecoveryCodeToSeed(std::string_view text);

}  // namespace compass::sync

#endif  // COMPASS_COMPONENTS_SYNC_CHAIN_H_
