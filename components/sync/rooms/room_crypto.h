// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_CRYPTO_H_
#define COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_CRYPTO_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/span.h"
#include "compass/components/sync/chain.h"

// Rooms' cryptography: the identity key, room key envelopes and record
// encryption. See docs/design/sync.md, "Rooms: crypto and links", and the
// vectors every client tests in common/rooms/vectors.json. The iOS twin is
// ios/CompassKit/Sources/CompassRooms/RoomCrypto.swift.
namespace compass::sync::rooms {

inline constexpr size_t kRoomIdSize = 16;
using RoomId = std::array<uint8_t, kRoomIdSize>;

// One epoch's room key (AES-256-GCM).
inline constexpr size_t kRoomKeySize = 32;
using RoomKey = std::array<uint8_t, kRoomKeySize>;

inline constexpr size_t kInviteSecretSize = 32;
using InviteSecret = std::array<uint8_t, kInviteSecretSize>;

inline constexpr size_t kX25519KeySize = 32;
using X25519Key = std::array<uint8_t, kX25519KeySize>;

// HKDF info for the identity key (salt empty, SHA-256, 32 bytes out).
inline constexpr char kIdentityInfo[] = "compass-identity-x25519";
// Prefix of an envelope's HPKE info.
inline constexpr char kEnvelopeInfoPrefix[] = "compass-room-key";
// Prefix of a record's AES-GCM associated data.
inline constexpr char kRecordAadPrefix[] = "compass-room-record";

// An envelope: HPKE enc (32 bytes) || the sealed room key (32 + 16 tag).
inline constexpr size_t kEnvelopeSize = 32 + kRoomKeySize + 16;
// A record's overhead: nonce (12) || ciphertext || tag (16).
inline constexpr size_t kRecordNonceSize = 12;
inline constexpr size_t kRecordOverhead = kRecordNonceSize + 16;

// An account's X25519 key pair. Members publish |public_key|; envelopes for
// them are sealed to it.
struct IdentityKey {
  // The X25519 private scalar as derived (X25519 clamps it when used).
  X25519Key private_key{};
  X25519Key public_key{};
};

// HKDF-SHA256(seed, salt="", info="compass-identity-x25519"[":" +
// profile_uuid]) as the private key; public = X25519(private, 9). An empty
// |profile_uuid| gives the chain's home account's key, as DeriveAuthKey().
IdentityKey DeriveIdentityKey(const Seed& seed,
                              std::string_view profile_uuid = {});

// The key pair for a private key (the public half recomputed).
IdentityKey IdentityKeyFromPrivate(const X25519Key& private_key);

RoomId GenerateRoomId();
RoomKey GenerateRoomKey();
InviteSecret GenerateInviteSecret();

// What the server stores for a link invite: SHA-256(secret), lowercase hex.
// The secret itself stays in the link.
std::string InviteSecretHash(const InviteSecret& secret);

// "compass-room-key" || room_id || epoch (uint32, big-endian).
std::vector<uint8_t> EnvelopeInfo(const RoomId& room_id, uint32_t epoch);

// "compass-room-record" || room_id || record_id (UTF-8) || epoch (uint32,
// big-endian).
std::vector<uint8_t> RecordAad(const RoomId& room_id,
                               std::string_view record_id,
                               uint32_t epoch);

// Seals |room_key| to a member's identity |recipient_public_key| with HPKE
// (RFC 9180) base mode, DHKEM(X25519, HKDF-SHA256), HKDF-SHA256,
// AES-256-GCM, info = EnvelopeInfo(), no AAD. Returns enc || ciphertext
// (kEnvelopeSize bytes), or empty if the public key is unusable.
std::vector<uint8_t> SealEnvelope(const X25519Key& recipient_public_key,
                                  const RoomId& room_id,
                                  uint32_t epoch,
                                  const RoomKey& room_key);

// As SealEnvelope(), with the ephemeral private key given (vectors only).
std::vector<uint8_t> SealEnvelopeWithEphemeralForTesting(
    const X25519Key& recipient_public_key,
    const RoomId& room_id,
    uint32_t epoch,
    const RoomKey& room_key,
    const X25519Key& ephemeral_private_key);

// The room key in |envelope|, or nullopt if it isn't for |identity|, this
// room and this epoch, or was tampered with.
std::optional<RoomKey> OpenEnvelope(const IdentityKey& identity,
                                    const RoomId& room_id,
                                    uint32_t epoch,
                                    base::span<const uint8_t> envelope);

// AES-256-GCM of |plaintext| (a serialized CompassSpacesSpecifics) under
// |room_key| with a random 12-byte nonce and AAD = RecordAad(). Returns
// nonce || ciphertext || tag.
std::vector<uint8_t> EncryptRecord(const RoomKey& room_key,
                                   const RoomId& room_id,
                                   std::string_view record_id,
                                   uint32_t epoch,
                                   base::span<const uint8_t> plaintext);

// As EncryptRecord(), with the nonce given (vectors only).
std::vector<uint8_t> EncryptRecordWithNonceForTesting(
    const RoomKey& room_key,
    const RoomId& room_id,
    std::string_view record_id,
    uint32_t epoch,
    base::span<const uint8_t> plaintext,
    base::span<const uint8_t, kRecordNonceSize> nonce);

// The plaintext of a record from EncryptRecord(), or nullopt if the key,
// room, record ID or epoch don't match or it was tampered with.
std::optional<std::vector<uint8_t>> DecryptRecord(
    const RoomKey& room_key,
    const RoomId& room_id,
    std::string_view record_id,
    uint32_t epoch,
    base::span<const uint8_t> sealed);

}  // namespace compass::sync::rooms

#endif  // COMPASS_COMPONENTS_SYNC_ROOMS_ROOM_CRYPTO_H_
