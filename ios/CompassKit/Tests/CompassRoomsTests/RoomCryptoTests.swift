// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation
import Testing

@testable import CompassRooms

/// common/rooms/vectors.json, shared with the desktop (or at
/// $COMPASS_ROOMS_VECTORS). Its deterministic envelopes are the bytes
/// BoringSSL seals; the "swift" ones were sealed by CryptoKit and the desktop
/// opens them.
enum RoomVectors {
  static var root: [String: Any] {
    var candidates: [URL] = []
    if let path = ProcessInfo.processInfo.environment["COMPASS_ROOMS_VECTORS"] {
      candidates.append(URL(fileURLWithPath: path))
    }
    // Tests/CompassRoomsTests/ → the package → ios/ → the repository.
    let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
      .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    candidates.append(repo.appendingPathComponent("common/rooms/vectors.json"))
    guard let url = candidates.first(where: { FileManager.default.fileExists(atPath: $0.path) }),
      let data = try? Data(contentsOf: url),
      let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    else { return [:] }
    return root
  }

  static func section(_ key: String) -> [[String: Any]] {
    root[key] as? [[String: Any]] ?? []
  }
}

extension Dictionary where Key == String, Value == Any {
  func string(_ key: String) -> String { self[key] as? String ?? "" }
  func bytes(_ key: String) -> Data { Data(hex: string(key)) ?? Data() }
  var epoch: UInt32 { UInt32((self["epoch"] as? NSNumber)?.uint64Value ?? 0) }
}

struct RoomCryptoTests {
  @Test func vectorsLoad() {
    for key in ["identity", "envelopes", "envelope_failures", "records", "record_failures",
                "invite_secret_hashes", "links", "invalid_links"] {
      #expect(!RoomVectors.section(key).isEmpty, "\(key)")
    }
  }

  @Test func identityVectors() throws {
    for v in RoomVectors.section("identity") {
      let key = RoomCrypto.identityKey(seed: v.bytes("seed"), profileUUID: v.string("profile_uuid"))
      #expect(key.privateKey.hexString(uppercase: false) == v.string("private_key"), "\(v.string("name"))")
      #expect(key.publicKey.hexString(uppercase: false) == v.string("public_key"), "\(v.string("name"))")
      #expect(try IdentityKey(privateKey: key.privateKey) == key)
    }
  }

  @Test func identityFromChain() throws {
    let chain = try Chain(seed: Chain.generateSeed())
    #expect(RoomCrypto.identityKey(chain: chain) == RoomCrypto.identityKey(seed: chain.seed))
    #expect(RoomCrypto.identityKey(chain: chain, profileUUID: "p") != RoomCrypto.identityKey(chain: chain))
    #expect(RoomCrypto.identityKey(chain: chain).privateKey != chain.authKey)
  }

  @Test func envelopeVectorsOpen() throws {
    var sealedByEach: Set<String> = []
    for v in RoomVectors.section("envelopes") {
      let me = try IdentityKey(privateKey: v.bytes("recipient_private_key"))
      #expect(me.publicKey == v.bytes("recipient_public_key"))
      #expect(RoomCrypto.envelopeInfo(roomID: v.bytes("room_id"), epoch: v.epoch) == v.bytes("info"))
      let key = RoomCrypto.openEnvelope(
        identity: me, roomID: v.bytes("room_id"), epoch: v.epoch, envelope: v.bytes("envelope"))
      #expect(key == v.bytes("room_key"), "\(v.string("name"))")
      sealedByEach.insert(v.string("sealed_by"))
    }
    #expect(sealedByEach == ["deterministic", "swift"])
  }

  @Test func envelopeFailureVectors() throws {
    for v in RoomVectors.section("envelope_failures") {
      let me = try IdentityKey(privateKey: v.bytes("recipient_private_key"))
      #expect(
        RoomCrypto.openEnvelope(
          identity: me, roomID: v.bytes("room_id"), epoch: v.epoch, envelope: v.bytes("envelope")) == nil,
        "\(v.string("name"))")
    }
  }

  @Test func envelopeRoundTrip() throws {
    let me = RoomCrypto.identityKey(seed: Chain.generateSeed(), profileUUID: "uuid")
    let room = RoomCrypto.generateRoomID()
    let key = RoomCrypto.generateRoomKey()
    let a = try RoomCrypto.sealEnvelope(recipientPublicKey: me.publicKey, roomID: room, epoch: 3, roomKey: key)
    let b = try RoomCrypto.sealEnvelope(recipientPublicKey: me.publicKey, roomID: room, epoch: 3, roomKey: key)
    #expect(a.count == RoomCrypto.envelopeLength)
    #expect(a != b)
    #expect(RoomCrypto.openEnvelope(identity: me, roomID: room, epoch: 3, envelope: a) == key)
    #expect(RoomCrypto.openEnvelope(identity: me, roomID: room, epoch: 3, envelope: b) == key)
    #expect(RoomCrypto.openEnvelope(identity: me, roomID: room, epoch: 4, envelope: a) == nil)
    let stranger = RoomCrypto.identityKey(seed: Chain.generateSeed())
    #expect(RoomCrypto.openEnvelope(identity: stranger, roomID: room, epoch: 3, envelope: a) == nil)
  }

  @Test func recordVectorsEncrypt() throws {
    for v in RoomVectors.section("records") {
      #expect(
        RoomCrypto.recordAAD(roomID: v.bytes("room_id"), recordID: v.string("record_id"), epoch: v.epoch)
          == v.bytes("aad"))
      let sealed = try RoomCrypto.encryptRecord(
        roomKey: v.bytes("room_key"), roomID: v.bytes("room_id"), recordID: v.string("record_id"),
        epoch: v.epoch, plaintext: v.bytes("plaintext"), nonce: v.bytes("nonce"))
      #expect(sealed == v.bytes("sealed"), "\(v.string("name"))")
    }
  }

  @Test func recordVectorsDecrypt() {
    for v in RoomVectors.section("records") {
      let plaintext = RoomCrypto.decryptRecord(
        roomKey: v.bytes("room_key"), roomID: v.bytes("room_id"), recordID: v.string("record_id"),
        epoch: v.epoch, sealed: v.bytes("sealed"))
      #expect(plaintext == v.bytes("plaintext"), "\(v.string("name"))")
    }
  }

  @Test func recordFailureVectors() {
    for v in RoomVectors.section("record_failures") {
      let plaintext = RoomCrypto.decryptRecord(
        roomKey: v.bytes("room_key"), roomID: v.bytes("room_id"), recordID: v.string("record_id"),
        epoch: v.epoch, sealed: v.bytes("sealed"))
      #expect(plaintext == nil, "\(v.string("name"))")
    }
  }

  @Test func recordRoundTripAndTampering() throws {
    let key = RoomCrypto.generateRoomKey()
    let room = RoomCrypto.generateRoomID()
    let plaintext = Data([1, 2, 3, 4, 5])
    let a = try RoomCrypto.encryptRecord(roomKey: key, roomID: room, recordID: "r", epoch: 9, plaintext: plaintext)
    let b = try RoomCrypto.encryptRecord(roomKey: key, roomID: room, recordID: "r", epoch: 9, plaintext: plaintext)
    #expect(a.count == plaintext.count + RoomCrypto.recordOverhead)
    #expect(a != b)
    #expect(RoomCrypto.decryptRecord(roomKey: key, roomID: room, recordID: "r", epoch: 9, sealed: a) == plaintext)
    #expect(RoomCrypto.decryptRecord(roomKey: key, roomID: RoomCrypto.generateRoomID(), recordID: "r", epoch: 9, sealed: a) == nil)
    #expect(RoomCrypto.decryptRecord(roomKey: key, roomID: room, recordID: "s", epoch: 9, sealed: a) == nil)
    #expect(RoomCrypto.decryptRecord(roomKey: key, roomID: room, recordID: "r", epoch: 10, sealed: a) == nil)
    for i in 0..<a.count {
      var tampered = a
      tampered[i] ^= 0x80
      #expect(RoomCrypto.decryptRecord(roomKey: key, roomID: room, recordID: "r", epoch: 9, sealed: tampered) == nil)
    }
  }

  @Test func inviteSecretHashVectors() {
    for v in RoomVectors.section("invite_secret_hashes") {
      #expect(RoomCrypto.inviteSecretHash(v.bytes("invite_secret")) == v.string("hash"))
    }
  }

  @Test func badLengthsThrow() {
    #expect(throws: RoomCryptoError.badLength) { try IdentityKey(privateKey: Data(count: 31)) }
    #expect(throws: RoomCryptoError.badLength) {
      try RoomCrypto.encryptRecord(roomKey: Data(count: 16), roomID: Data(count: 16), recordID: "r", epoch: 0, plaintext: Data())
    }
  }
}

struct RoomInviteLinkTests {
  static func expected(_ v: [String: Any]) -> RoomInviteLink {
    let e = v["expected"] as? [String: Any] ?? [:]
    return RoomInviteLink(
      serverURL: e.string("server_url"), roomID: e.bytes("room_id"), inviteID: e.string("invite_id"),
      inviteSecret: e.bytes("invite_secret"), epochKey: e.bytes("epoch_key"), epoch: e.epoch,
      name: e.string("name"), kind: RoomKind(rawValue: e.string("kind")) ?? .space)
  }

  @Test func vectorsParse() {
    for v in RoomVectors.section("links") {
      #expect(RoomInviteLink(link: v.string("link")) == Self.expected(v), "\(v.string("name"))")
    }
  }

  @Test func vectorsEncode() {
    for v in RoomVectors.section("links") where v["encodes"] as? Bool == true {
      #expect(Self.expected(v).json == v.string("json"), "\(v.string("name"))")
      #expect(Self.expected(v).link == v.string("link"), "\(v.string("name"))")
    }
  }

  @Test func invalidVectors() {
    for v in RoomVectors.section("invalid_links") {
      #expect(RoomInviteLink(link: v.string("link")) == nil, "\(v.string("name"))")
    }
  }

  @Test func roundTripRandom() throws {
    let link = RoomInviteLink(
      serverURL: "https://sync.example.com/v2", roomID: RoomCrypto.generateRoomID(), inviteID: "abc",
      inviteSecret: RoomCrypto.generateInviteSecret(), epochKey: RoomCrypto.generateRoomKey(), epoch: 42,
      name: "Board \"🧭\"", kind: .easel)
    let text = try #require(link.link)
    #expect(RoomInviteLink(link: text) == link)
    let deep = RoomInviteLink.deepLinkPrefix + text.dropFirst(RoomInviteLink.prefix.count)
    #expect(RoomInviteLink(link: deep) == link)
  }

  @Test func encodeRefusesInvalid() {
    var link = RoomInviteLink(
      serverURL: "http://sync.example.com", roomID: Data(count: 16), inviteID: "abc",
      inviteSecret: Data(count: 32), epochKey: Data(count: 32), epoch: 0, name: "", kind: .space)
    #expect(link.link == nil)
    link.serverURL = "https://sync.example.com"
    #expect(link.link != nil)
    link.inviteID = ""
    #expect(link.link == nil)
    link.inviteID = "abc"
    link.name = String(repeating: "a", count: RoomInviteLink.maxNameLength + 1)
    #expect(link.link == nil)
    link.name = ""
    link.roomID = Data(count: 15)
    #expect(link.link == nil)
  }
}
