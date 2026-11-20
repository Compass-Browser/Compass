// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/rooms/room_crypto.h"

#include "base/containers/extend.h"
#include "base/numerics/byte_conversions.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "crypto/aead.h"
#include "crypto/hash.h"
#include "crypto/kdf.h"
#include "crypto/random.h"
#include "third_party/boringssl/src/include/openssl/curve25519.h"
#include "third_party/boringssl/src/include/openssl/hpke.h"

namespace compass::sync::rooms {
namespace {

std::vector<uint8_t> Seal(const X25519Key& recipient_public_key,
                          const RoomId& room_id,
                          uint32_t epoch,
                          const RoomKey& room_key,
                          const X25519Key* ephemeral_private_key) {
  const std::vector<uint8_t> info = EnvelopeInfo(room_id, epoch);
  bssl::ScopedEVP_HPKE_CTX ctx;
  std::vector<uint8_t> out(EVP_HPKE_MAX_ENC_LENGTH);
  size_t enc_len = 0;
  const int ok =
      ephemeral_private_key
          ? EVP_HPKE_CTX_setup_sender_with_seed_for_testing(
                ctx.get(), out.data(), &enc_len, out.size(),
                EVP_hpke_x25519_hkdf_sha256(), EVP_hpke_hkdf_sha256(),
                EVP_hpke_aes_256_gcm(), recipient_public_key.data(),
                recipient_public_key.size(), info.data(), info.size(),
                ephemeral_private_key->data(), ephemeral_private_key->size())
          : EVP_HPKE_CTX_setup_sender(
                ctx.get(), out.data(), &enc_len, out.size(),
                EVP_hpke_x25519_hkdf_sha256(), EVP_hpke_hkdf_sha256(),
                EVP_hpke_aes_256_gcm(), recipient_public_key.data(),
                recipient_public_key.size(), info.data(), info.size());
  if (!ok || enc_len != kX25519KeySize) {
    return {};
  }
  out.resize(kEnvelopeSize);
  size_t sealed_len = 0;
  const base::span<uint8_t> rest = base::span(out).subspan(enc_len);
  if (!EVP_HPKE_CTX_seal(ctx.get(), rest.data(), &sealed_len, rest.size(),
                         room_key.data(), room_key.size(), /*ad=*/nullptr, 0) ||
      enc_len + sealed_len != kEnvelopeSize) {
    return {};
  }
  return out;
}

std::vector<uint8_t> EncryptWithNonce(const RoomKey& room_key,
                                      const RoomId& room_id,
                                      std::string_view record_id,
                                      uint32_t epoch,
                                      base::span<const uint8_t> plaintext,
                                      base::span<const uint8_t> nonce) {
  std::vector<uint8_t> out(nonce.begin(), nonce.end());
  base::Extend(
      out, crypto::aead::Seal(crypto::aead::AES_256_GCM, room_key, plaintext,
                              nonce, RecordAad(room_id, record_id, epoch)));
  return out;
}

}  // namespace

IdentityKey DeriveIdentityKey(const Seed& seed, std::string_view profile_uuid) {
  const std::string info =
      profile_uuid.empty() ? std::string(kIdentityInfo)
                           : base::StrCat({kIdentityInfo, ":", profile_uuid});
  return IdentityKeyFromPrivate(crypto::kdf::Hkdf<kX25519KeySize>(
      crypto::hash::kSha256, seed, /*salt=*/base::span<const uint8_t>(),
      base::as_byte_span(info)));
}

IdentityKey IdentityKeyFromPrivate(const X25519Key& private_key) {
  IdentityKey key;
  key.private_key = private_key;
  X25519_public_from_private(key.public_key.data(), private_key.data());
  return key;
}

RoomId GenerateRoomId() {
  return crypto::RandBytesAsArray<kRoomIdSize>();
}

RoomKey GenerateRoomKey() {
  return crypto::RandBytesAsArray<kRoomKeySize>();
}

InviteSecret GenerateInviteSecret() {
  return crypto::RandBytesAsArray<kInviteSecretSize>();
}

std::string InviteSecretHash(const InviteSecret& secret) {
  return base::HexEncodeLower(crypto::hash::Sha256(secret));
}

std::vector<uint8_t> EnvelopeInfo(const RoomId& room_id, uint32_t epoch) {
  std::vector<uint8_t> info;
  base::Extend(info, base::as_byte_span(std::string_view(kEnvelopeInfoPrefix)));
  base::Extend(info, room_id);
  base::Extend(info, base::U32ToBigEndian(epoch));
  return info;
}

std::vector<uint8_t> RecordAad(const RoomId& room_id,
                               std::string_view record_id,
                               uint32_t epoch) {
  std::vector<uint8_t> aad;
  base::Extend(aad, base::as_byte_span(std::string_view(kRecordAadPrefix)));
  base::Extend(aad, room_id);
  base::Extend(aad, base::as_byte_span(record_id));
  base::Extend(aad, base::U32ToBigEndian(epoch));
  return aad;
}

std::vector<uint8_t> SealEnvelope(const X25519Key& recipient_public_key,
                                  const RoomId& room_id,
                                  uint32_t epoch,
                                  const RoomKey& room_key) {
  return Seal(recipient_public_key, room_id, epoch, room_key, nullptr);
}

std::vector<uint8_t> SealEnvelopeWithEphemeralForTesting(
    const X25519Key& recipient_public_key,
    const RoomId& room_id,
    uint32_t epoch,
    const RoomKey& room_key,
    const X25519Key& ephemeral_private_key) {
  return Seal(recipient_public_key, room_id, epoch, room_key,
              &ephemeral_private_key);
}

std::optional<RoomKey> OpenEnvelope(const IdentityKey& identity,
                                    const RoomId& room_id,
                                    uint32_t epoch,
                                    base::span<const uint8_t> envelope) {
  if (envelope.size() != kEnvelopeSize) {
    return std::nullopt;
  }
  bssl::ScopedEVP_HPKE_KEY key;
  if (!EVP_HPKE_KEY_init(key.get(), EVP_hpke_x25519_hkdf_sha256(),
                         identity.private_key.data(),
                         identity.private_key.size())) {
    return std::nullopt;
  }
  const auto [enc, sealed] = envelope.split_at(kX25519KeySize);
  const std::vector<uint8_t> info = EnvelopeInfo(room_id, epoch);
  bssl::ScopedEVP_HPKE_CTX ctx;
  if (!EVP_HPKE_CTX_setup_recipient(
          ctx.get(), key.get(), EVP_hpke_hkdf_sha256(), EVP_hpke_aes_256_gcm(),
          enc.data(), enc.size(), info.data(), info.size())) {
    return std::nullopt;
  }
  RoomKey room_key{};
  size_t out_len = 0;
  if (!EVP_HPKE_CTX_open(ctx.get(), room_key.data(), &out_len, room_key.size(),
                         sealed.data(), sealed.size(),
                         /*ad=*/nullptr, 0) ||
      out_len != kRoomKeySize) {
    return std::nullopt;
  }
  return room_key;
}

std::vector<uint8_t> EncryptRecord(const RoomKey& room_key,
                                   const RoomId& room_id,
                                   std::string_view record_id,
                                   uint32_t epoch,
                                   base::span<const uint8_t> plaintext) {
  const auto nonce = crypto::RandBytesAsArray<kRecordNonceSize>();
  return EncryptWithNonce(room_key, room_id, record_id, epoch, plaintext,
                          nonce);
}

std::vector<uint8_t> EncryptRecordWithNonceForTesting(
    const RoomKey& room_key,
    const RoomId& room_id,
    std::string_view record_id,
    uint32_t epoch,
    base::span<const uint8_t> plaintext,
    base::span<const uint8_t, kRecordNonceSize> nonce) {
  return EncryptWithNonce(room_key, room_id, record_id, epoch, plaintext,
                          nonce);
}

std::optional<std::vector<uint8_t>> DecryptRecord(
    const RoomKey& room_key,
    const RoomId& room_id,
    std::string_view record_id,
    uint32_t epoch,
    base::span<const uint8_t> sealed) {
  if (sealed.size() < kRecordOverhead) {
    return std::nullopt;
  }
  const auto [nonce, ciphertext] = sealed.split_at(kRecordNonceSize);
  return crypto::aead::Open(crypto::aead::AES_256_GCM, room_key, ciphertext,
                            nonce, RecordAad(room_id, record_id, epoch));
}

}  // namespace compass::sync::rooms
