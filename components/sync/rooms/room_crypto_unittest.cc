// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "compass/components/sync/rooms/room_crypto.h"

#include "base/strings/string_number_conversions.h"
#include "compass/components/sync/rooms/room_test_vectors.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace compass::sync::rooms {
namespace {

using test::Array;
using test::Bytes;
using test::Epoch;
using test::Section;
using test::Str;

TEST(RoomCryptoTest, IdentityVectors) {
  for (const base::DictValue& v : Section("identity")) {
    SCOPED_TRACE(Str(v, "name"));
    const IdentityKey key =
        DeriveIdentityKey(Array<kSeedSize>(v, "seed"), Str(v, "profile_uuid"));
    EXPECT_EQ(base::HexEncodeLower(key.private_key), Str(v, "private_key"));
    EXPECT_EQ(base::HexEncodeLower(key.public_key), Str(v, "public_key"));
    EXPECT_EQ(IdentityKeyFromPrivate(key.private_key).public_key,
              key.public_key);
  }
}

TEST(RoomCryptoTest, IdentityDiffersPerProfileAndFromAuthKey) {
  const Seed seed = GenerateSeed();
  const IdentityKey home = DeriveIdentityKey(seed);
  EXPECT_EQ(home.private_key, DeriveIdentityKey(seed).private_key);
  EXPECT_NE(home.private_key, DeriveIdentityKey(seed, "p").private_key);
  EXPECT_NE(home.private_key, DeriveAuthKey(seed));
}

TEST(RoomCryptoTest, EnvelopeVectorsOpen) {
  for (const base::DictValue& v : Section("envelopes")) {
    SCOPED_TRACE(Str(v, "name"));
    const IdentityKey me =
        IdentityKeyFromPrivate(Array<32>(v, "recipient_private_key"));
    EXPECT_EQ(base::HexEncodeLower(me.public_key),
              Str(v, "recipient_public_key"));
    const RoomId room = Array<kRoomIdSize>(v, "room_id");
    const uint32_t epoch = Epoch(v);
    EXPECT_EQ(base::HexEncodeLower(EnvelopeInfo(room, epoch)), Str(v, "info"));
    const std::optional<RoomKey> key =
        OpenEnvelope(me, room, epoch, Bytes(v, "envelope"));
    ASSERT_TRUE(key);
    EXPECT_EQ(base::HexEncodeLower(*key), Str(v, "room_key"));
  }
}

// The deterministic envelopes are what BoringSSL seals with that ephemeral
// key, byte for byte; iOS opens them.
TEST(RoomCryptoTest, EnvelopeVectorsSeal) {
  int sealed = 0;
  for (const base::DictValue& v : Section("envelopes")) {
    if (!v.FindString("ephemeral_private_key")) {
      continue;  // Swift-sealed, random ephemeral.
    }
    SCOPED_TRACE(Str(v, "name"));
    EXPECT_EQ(base::HexEncodeLower(SealEnvelopeWithEphemeralForTesting(
                  Array<32>(v, "recipient_public_key"),
                  Array<kRoomIdSize>(v, "room_id"), Epoch(v),
                  Array<kRoomKeySize>(v, "room_key"),
                  Array<32>(v, "ephemeral_private_key"))),
              Str(v, "envelope"));
    ++sealed;
  }
  EXPECT_GE(sealed, 2);
}

TEST(RoomCryptoTest, EnvelopeFailureVectors) {
  for (const base::DictValue& v : Section("envelope_failures")) {
    SCOPED_TRACE(Str(v, "name"));
    EXPECT_FALSE(OpenEnvelope(
        IdentityKeyFromPrivate(Array<32>(v, "recipient_private_key")),
        Array<kRoomIdSize>(v, "room_id"), Epoch(v), Bytes(v, "envelope")));
  }
}

TEST(RoomCryptoTest, EnvelopeRoundTrip) {
  const IdentityKey me = DeriveIdentityKey(GenerateSeed(), "uuid");
  const RoomId room = GenerateRoomId();
  const RoomKey key = GenerateRoomKey();
  const std::vector<uint8_t> a = SealEnvelope(me.public_key, room, 3, key);
  const std::vector<uint8_t> b = SealEnvelope(me.public_key, room, 3, key);
  ASSERT_EQ(a.size(), kEnvelopeSize);
  EXPECT_NE(a, b);  // A fresh ephemeral key each time.
  EXPECT_EQ(OpenEnvelope(me, room, 3, a), key);
  EXPECT_EQ(OpenEnvelope(me, room, 3, b), key);
  EXPECT_FALSE(OpenEnvelope(me, room, 4, a));
  EXPECT_FALSE(OpenEnvelope(DeriveIdentityKey(GenerateSeed()), room, 3, a));
}

TEST(RoomCryptoTest, EnvelopeToLowOrderKeyFails) {
  // X25519 with the all-zero point gives an all-zero secret, which HPKE
  // refuses.
  EXPECT_TRUE(SealEnvelope(X25519Key{}, GenerateRoomId(), 1, GenerateRoomKey())
                  .empty());
}

TEST(RoomCryptoTest, RecordVectorsEncrypt) {
  for (const base::DictValue& v : Section("records")) {
    SCOPED_TRACE(Str(v, "name"));
    const RoomId room = Array<kRoomIdSize>(v, "room_id");
    const std::string record_id = Str(v, "record_id");
    EXPECT_EQ(base::HexEncodeLower(RecordAad(room, record_id, Epoch(v))),
              Str(v, "aad"));
    const auto nonce = Array<kRecordNonceSize>(v, "nonce");
    EXPECT_EQ(base::HexEncodeLower(EncryptRecordWithNonceForTesting(
                  Array<kRoomKeySize>(v, "room_key"), room, record_id, Epoch(v),
                  Bytes(v, "plaintext"), nonce)),
              Str(v, "sealed"));
  }
}

TEST(RoomCryptoTest, RecordVectorsDecrypt) {
  for (const base::DictValue& v : Section("records")) {
    SCOPED_TRACE(Str(v, "name"));
    EXPECT_EQ(DecryptRecord(Array<kRoomKeySize>(v, "room_key"),
                            Array<kRoomIdSize>(v, "room_id"),
                            Str(v, "record_id"), Epoch(v), Bytes(v, "sealed")),
              Bytes(v, "plaintext"));
  }
}

TEST(RoomCryptoTest, RecordFailureVectors) {
  for (const base::DictValue& v : Section("record_failures")) {
    SCOPED_TRACE(Str(v, "name"));
    EXPECT_FALSE(DecryptRecord(
        Array<kRoomKeySize>(v, "room_key"), Array<kRoomIdSize>(v, "room_id"),
        Str(v, "record_id"), Epoch(v), Bytes(v, "sealed")));
  }
}

TEST(RoomCryptoTest, RecordRoundTripAndTampering) {
  const RoomKey key = GenerateRoomKey();
  const RoomId room = GenerateRoomId();
  const std::vector<uint8_t> plaintext = {1, 2, 3, 4, 5};
  const std::vector<uint8_t> a = EncryptRecord(key, room, "r", 9, plaintext);
  const std::vector<uint8_t> b = EncryptRecord(key, room, "r", 9, plaintext);
  EXPECT_EQ(a.size(), plaintext.size() + kRecordOverhead);
  EXPECT_NE(a, b);  // Random nonces.
  EXPECT_EQ(DecryptRecord(key, room, "r", 9, a), plaintext);
  EXPECT_FALSE(DecryptRecord(key, GenerateRoomId(), "r", 9, a));
  EXPECT_FALSE(DecryptRecord(key, room, "s", 9, a));
  EXPECT_FALSE(DecryptRecord(key, room, "r", 10, a));
  EXPECT_FALSE(DecryptRecord(GenerateRoomKey(), room, "r", 9, a));
  for (size_t i = 0; i < a.size(); ++i) {
    std::vector<uint8_t> tampered = a;
    tampered[i] ^= 0x80;
    EXPECT_FALSE(DecryptRecord(key, room, "r", 9, tampered)) << i;
  }
  EXPECT_FALSE(DecryptRecord(key, room, "r", 9, base::span(a).first(10u)));
}

TEST(RoomCryptoTest, InviteSecretHashVectors) {
  for (const base::DictValue& v : Section("invite_secret_hashes")) {
    EXPECT_EQ(InviteSecretHash(Array<kInviteSecretSize>(v, "invite_secret")),
              Str(v, "hash"));
  }
}

TEST(RoomCryptoTest, GeneratedValuesDiffer) {
  EXPECT_NE(GenerateRoomId(), GenerateRoomId());
  EXPECT_NE(GenerateRoomKey(), GenerateRoomKey());
  EXPECT_NE(GenerateInviteSecret(), GenerateInviteSecret());
}

}  // namespace
}  // namespace compass::sync::rooms
