// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Writes common/rooms/vectors.json, the rooms crypto and invite link vectors
// that components/sync/rooms (C++) and CompassKit's CompassRooms (Swift) both
// test (docs/design/sync.md, "Rooms: crypto and links"):
//
//   swift common/rooms/make_vectors.swift > common/rooms/vectors.json
//
// It doesn't use either client's code. The deterministic envelopes come from
// an HPKE base mode written out here from RFC 9180 on CryptoKit's primitives,
// with a fixed ephemeral key; the C++ test seals the same bytes with
// BoringSSL. The "swift_sealed" envelopes come from CryptoKit's own HPKE,
// which picks its ephemeral key at random, so they change on every run: both
// clients only open them.

import CryptoKit
import Foundation

// MARK: Helpers

func hex(_ data: Data) -> String { data.map { String(format: "%02x", $0) }.joined() }
func unhex(_ s: String) -> Data {
  var out = Data()
  var i = s.startIndex
  while i < s.endIndex {
    let j = s.index(i, offsetBy: 2)
    out.append(UInt8(s[i..<j], radix: 16)!)
    i = j
  }
  return out
}
func b64url(_ data: Data) -> String {
  data.base64EncodedString().replacingOccurrences(of: "+", with: "-")
    .replacingOccurrences(of: "/", with: "_").replacingOccurrences(of: "=", with: "")
}
func bytes(_ data: some ContiguousBytes) -> Data { data.withUnsafeBytes { Data($0) } }
func be32(_ v: UInt32) -> Data { Data([UInt8(v >> 24), UInt8(v >> 16 & 0xff), UInt8(v >> 8 & 0xff), UInt8(v & 0xff)]) }
func be16(_ v: UInt16) -> Data { Data([UInt8(v >> 8), UInt8(v & 0xff)]) }
func sha256(_ data: Data) -> Data { Data(SHA256.hash(data: data)) }
func flip(_ data: Data, at index: Int) -> Data {
  var d = data
  d[d.startIndex + index] ^= 0x01
  return d
}

// MARK: The scheme

func identityPrivate(seed: Data, profileUUID: String) -> Data {
  let info = profileUUID.isEmpty ? "compass-identity-x25519" : "compass-identity-x25519:" + profileUUID
  return bytes(HKDF<SHA256>.deriveKey(
    inputKeyMaterial: SymmetricKey(data: seed), salt: Data(), info: Data(info.utf8),
    outputByteCount: 32))
}
func x25519Public(_ priv: Data) -> Data {
  try! Curve25519.KeyAgreement.PrivateKey(rawRepresentation: priv).publicKey.rawRepresentation
}
func envelopeInfo(room: Data, epoch: UInt32) -> Data { Data("compass-room-key".utf8) + room + be32(epoch) }
func recordAAD(room: Data, recordID: String, epoch: UInt32) -> Data {
  Data("compass-room-record".utf8) + room + Data(recordID.utf8) + be32(epoch)
}

// RFC 9180 base mode: DHKEM(X25519, HKDF-SHA256) 0x0020, HKDF-SHA256 0x0001,
// AES-256-GCM 0x0002.
func labeledExtract(_ suite: Data, salt: Data, label: String, ikm: Data) -> Data {
  let input = Data("HPKE-v1".utf8) + suite + Data(label.utf8) + ikm
  return Data(HKDF<SHA256>.extract(inputKeyMaterial: SymmetricKey(data: input), salt: salt))
}
func labeledExpand(_ suite: Data, prk: Data, label: String, info: Data, length: Int) -> Data {
  let labeled = be16(UInt16(length)) + Data("HPKE-v1".utf8) + suite + Data(label.utf8) + info
  return bytes(HKDF<SHA256>.expand(pseudoRandomKey: prk, info: labeled, outputByteCount: length))
}
func hpkeSeal(recipientPublic pkR: Data, ephemeralPrivate skE: Data, info: Data, plaintext: Data) -> Data {
  let kemSuite = Data("KEM".utf8) + be16(0x0020)
  let ephemeral = try! Curve25519.KeyAgreement.PrivateKey(rawRepresentation: skE)
  let enc = ephemeral.publicKey.rawRepresentation
  let dh = bytes(try! ephemeral.sharedSecretFromKeyAgreement(
    with: Curve25519.KeyAgreement.PublicKey(rawRepresentation: pkR)))
  let eaePRK = labeledExtract(kemSuite, salt: Data(), label: "eae_prk", ikm: dh)
  let shared = labeledExpand(kemSuite, prk: eaePRK, label: "shared_secret", info: enc + pkR, length: 32)

  let suite = Data("HPKE".utf8) + be16(0x0020) + be16(0x0001) + be16(0x0002)
  let pskIDHash = labeledExtract(suite, salt: Data(), label: "psk_id_hash", ikm: Data())
  let infoHash = labeledExtract(suite, salt: Data(), label: "info_hash", ikm: info)
  let context = Data([0]) + pskIDHash + infoHash
  let secret = labeledExtract(suite, salt: shared, label: "secret", ikm: Data())
  let key = labeledExpand(suite, prk: secret, label: "key", info: context, length: 32)
  let nonce = labeledExpand(suite, prk: secret, label: "base_nonce", info: context, length: 12)
  let box = try! AES.GCM.seal(plaintext, using: SymmetricKey(data: key), nonce: AES.GCM.Nonce(data: nonce))
  return enc + box.ciphertext + box.tag
}
// CryptoKit has no preset for X25519 with AES-256-GCM; it's built from parts.
let suite = HPKE.Ciphersuite(kem: .Curve25519_HKDF_SHA256, kdf: .HKDF_SHA256, aead: .AES_GCM_256)
func cryptoKitSeal(recipientPublic: Data, info: Data, plaintext: Data) -> Data {
  var sender = try! HPKE.Sender(
    recipientKey: Curve25519.KeyAgreement.PublicKey(rawRepresentation: recipientPublic),
    ciphersuite: suite, info: info)
  return sender.encapsulatedKey + (try! sender.seal(plaintext))
}
func cryptoKitOpen(recipientPrivate: Data, info: Data, envelope: Data) -> Data? {
  guard envelope.count > 32 else { return nil }
  let enc = envelope.prefix(32), ct = envelope.dropFirst(32)
  guard var recipient = try? HPKE.Recipient(
    privateKey: Curve25519.KeyAgreement.PrivateKey(rawRepresentation: recipientPrivate),
    ciphersuite: suite, info: info, encapsulatedKey: Data(enc))
  else { return nil }
  return try? recipient.open(Data(ct))
}
func sealRecord(key: Data, room: Data, recordID: String, epoch: UInt32, nonce: Data, plaintext: Data) -> Data {
  let box = try! AES.GCM.seal(
    plaintext, using: SymmetricKey(data: key), nonce: AES.GCM.Nonce(data: nonce),
    authenticating: recordAAD(room: room, recordID: recordID, epoch: epoch))
  return nonce + box.ciphertext + box.tag
}

// The link's JSON: compact, keys sorted, strings escaping only '"', '\' and
// control characters.
func jsonStringUTF8(_ s: String) -> String {
  var out = Data("\"".utf8)
  for b in s.utf8 {
    switch b {
    case 0x22: out += Data("\\\"".utf8)
    case 0x5c: out += Data("\\\\".utf8)
    case 0x08: out += Data("\\b".utf8)
    case 0x0c: out += Data("\\f".utf8)
    case 0x0a: out += Data("\\n".utf8)
    case 0x0d: out += Data("\\r".utf8)
    case 0x09: out += Data("\\t".utf8)
    case ..<0x20: out += Data(String(format: "\\u%04x", b).utf8)
    default: out.append(b)
    }
  }
  out += Data("\"".utf8)
  return String(decoding: out, as: UTF8.self)
}

struct Link {
  var s: String, r: Data, i: String, x: Data, k: Data, e: UInt32, n: String, t: String
  var json: String {
    "{\"e\":\(e),\"i\":\(jsonStringUTF8(i)),\"k\":\(jsonStringUTF8(b64url(k))),\"n\":\(jsonStringUTF8(n)),"
      + "\"r\":\(jsonStringUTF8(b64url(r))),\"s\":\(jsonStringUTF8(s)),\"t\":\(jsonStringUTF8(t)),"
      + "\"x\":\(jsonStringUTF8(b64url(x)))}"
  }
  var expected: [String: Any] {
    ["server_url": s, "room_id": hex(r), "invite_id": i, "invite_secret": hex(x),
     "epoch_key": hex(k), "epoch": Int(e), "name": n, "kind": t]
  }
}
func fragmentLink(_ json: String, prefix: String = "https://compassbrowser.org/join#") -> String {
  prefix + b64url(Data(json.utf8))
}

// MARK: Inputs

let seedA = unhex("000102030405060708090a0b0c0d0e0f")
let seedB = unhex("f0e1d2c3b4a5968778695a4b3c2d1e0f")
let profileUUID = "3f2a8c1e-5b7d-4e9a-8c6f-1d2e3f4a5b6c"
let roomA = unhex("00112233445566778899aabbccddeeff")
let roomB = unhex("ffeeddccbbaa99887766554433221100")
let roomKey = unhex("a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebf")
let otherKey = unhex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedf")
let ephemeral1 = sha256(Data("compass-rooms-vectors ephemeral 1".utf8))
let ephemeral2 = sha256(Data("compass-rooms-vectors ephemeral 2".utf8))
let recordID = "c3d2a1b0-9f8e-4d7c-8b6a-594837261504"
let nonce1 = unhex("000102030405060708090a0b")
let nonce2 = unhex("f0f1f2f3f4f5f6f7f8f9fafb")
let inviteSecret = unhex("5ec2e7000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c")

// MARK: Vectors

var identity: [[String: Any]] = []
for (name, seed, uuid) in [("home_a", seedA, ""), ("profile_a", seedA, profileUUID), ("home_b", seedB, "")] {
  let priv = identityPrivate(seed: seed, profileUUID: uuid)
  identity.append(["name": name, "seed": hex(seed), "profile_uuid": uuid,
                   "private_key": hex(priv), "public_key": hex(x25519Public(priv))])
}

let recipientA = identityPrivate(seed: seedA, profileUUID: "")
let recipientProfile = identityPrivate(seed: seedA, profileUUID: profileUUID)
let recipientB = identityPrivate(seed: seedB, profileUUID: "")

var envelopes: [[String: Any]] = []
var envelopeFailures: [[String: Any]] = []
func addEnvelope(_ name: String, recipient: Data, room: Data, epoch: UInt32, key: Data, ephemeral: Data?) -> Data {
  let info = envelopeInfo(room: room, epoch: epoch)
  let sealed = ephemeral.map { hpkeSeal(recipientPublic: x25519Public(recipient), ephemeralPrivate: $0, info: info, plaintext: key) }
    ?? cryptoKitSeal(recipientPublic: x25519Public(recipient), info: info, plaintext: key)
  precondition(cryptoKitOpen(recipientPrivate: recipient, info: info, envelope: sealed) == key)
  var v: [String: Any] = [
    "name": name, "recipient_private_key": hex(recipient), "recipient_public_key": hex(x25519Public(recipient)),
    "room_id": hex(room), "epoch": Int(epoch), "room_key": hex(key), "info": hex(info),
    "envelope": hex(sealed), "sealed_by": ephemeral == nil ? "swift" : "deterministic"]
  if let ephemeral { v["ephemeral_private_key"] = hex(ephemeral) }
  envelopes.append(v)
  return sealed
}
func addEnvelopeFailure(_ name: String, recipient: Data, room: Data, epoch: UInt32, envelope: Data) {
  precondition(cryptoKitOpen(recipientPrivate: recipient, info: envelopeInfo(room: room, epoch: epoch), envelope: envelope) == nil)
  envelopeFailures.append(["name": name, "recipient_private_key": hex(recipient), "room_id": hex(room),
                           "epoch": Int(epoch), "envelope": hex(envelope)])
}

let det1 = addEnvelope("deterministic_epoch_1", recipient: recipientA, room: roomA, epoch: 1, key: roomKey, ephemeral: ephemeral1)
_ = addEnvelope("deterministic_profile_max_epoch", recipient: recipientProfile, room: roomB, epoch: 0xffff_ffff, key: otherKey, ephemeral: ephemeral2)
let swift1 = addEnvelope("swift_sealed_epoch_7", recipient: recipientB, room: roomA, epoch: 7, key: roomKey, ephemeral: nil)
_ = addEnvelope("swift_sealed_profile_epoch_0", recipient: recipientProfile, room: roomB, epoch: 0, key: otherKey, ephemeral: nil)

addEnvelopeFailure("wrong_epoch", recipient: recipientA, room: roomA, epoch: 2, envelope: det1)
addEnvelopeFailure("wrong_room", recipient: recipientA, room: roomB, epoch: 1, envelope: det1)
addEnvelopeFailure("wrong_recipient", recipient: recipientB, room: roomA, epoch: 1, envelope: det1)
addEnvelopeFailure("other_account_of_same_seed", recipient: recipientProfile, room: roomA, epoch: 1, envelope: det1)
addEnvelopeFailure("flipped_enc", recipient: recipientA, room: roomA, epoch: 1, envelope: flip(det1, at: 3))
addEnvelopeFailure("flipped_ciphertext", recipient: recipientA, room: roomA, epoch: 1, envelope: flip(det1, at: 40))
addEnvelopeFailure("flipped_tag", recipient: recipientA, room: roomA, epoch: 1, envelope: flip(det1, at: 79))
addEnvelopeFailure("truncated", recipient: recipientA, room: roomA, epoch: 1, envelope: det1.prefix(79))
addEnvelopeFailure("extended", recipient: recipientA, room: roomA, epoch: 1, envelope: det1 + Data([0]))
addEnvelopeFailure("swift_sealed_wrong_epoch", recipient: recipientB, room: roomA, epoch: 8, envelope: swift1)

var records: [[String: Any]] = []
var recordFailures: [[String: Any]] = []
func addRecord(_ name: String, key: Data, room: Data, id: String, epoch: UInt32, nonce: Data, plaintext: Data) -> Data {
  let sealed = sealRecord(key: key, room: room, recordID: id, epoch: epoch, nonce: nonce, plaintext: plaintext)
  records.append(["name": name, "room_key": hex(key), "room_id": hex(room), "record_id": id, "epoch": Int(epoch),
                  "nonce": hex(nonce), "plaintext": hex(plaintext),
                  "aad": hex(recordAAD(room: room, recordID: id, epoch: epoch)), "sealed": hex(sealed)])
  return sealed
}
func addRecordFailure(_ name: String, key: Data, room: Data, id: String, epoch: UInt32, sealed: Data) {
  let ok: Bool = {
    guard sealed.count >= 28 else { return false }
    guard let box = try? AES.GCM.SealedBox(combined: sealed) else { return false }
    return (try? AES.GCM.open(box, using: SymmetricKey(data: key),
                              authenticating: recordAAD(room: room, recordID: id, epoch: epoch))) != nil
  }()
  precondition(!ok)
  recordFailures.append(["name": name, "room_key": hex(key), "room_id": hex(room), "record_id": id,
                         "epoch": Int(epoch), "sealed": hex(sealed)])
}
// A serialized CompassSpacesSpecifics is just bytes at this layer.
let specifics = Data("\n$c3d2a1b0-9f8e-4d7c-8b6a-594837261504\u{12}\u{07}Reading".utf8)
let rec1 = addRecord("node", key: roomKey, room: roomA, id: recordID, epoch: 1, nonce: nonce1, plaintext: specifics)
_ = addRecord("empty_plaintext", key: otherKey, room: roomB, id: "e", epoch: 0xffff_ffff, nonce: nonce2, plaintext: Data())
_ = addRecord("unicode_record_id", key: roomKey, room: roomA, id: "easel-✏️-1", epoch: 3, nonce: nonce2,
              plaintext: unhex("00ff00ff"))
addRecordFailure("wrong_room", key: roomKey, room: roomB, id: recordID, epoch: 1, sealed: rec1)
addRecordFailure("wrong_record_id", key: roomKey, room: roomA, id: "c3d2a1b0-9f8e-4d7c-8b6a-594837261505", epoch: 1, sealed: rec1)
addRecordFailure("wrong_epoch", key: roomKey, room: roomA, id: recordID, epoch: 2, sealed: rec1)
addRecordFailure("wrong_key", key: otherKey, room: roomA, id: recordID, epoch: 1, sealed: rec1)
addRecordFailure("flipped_nonce", key: roomKey, room: roomA, id: recordID, epoch: 1, sealed: flip(rec1, at: 0))
addRecordFailure("flipped_ciphertext", key: roomKey, room: roomA, id: recordID, epoch: 1, sealed: flip(rec1, at: 14))
addRecordFailure("flipped_tag", key: roomKey, room: roomA, id: recordID, epoch: 1, sealed: flip(rec1, at: rec1.count - 1))
addRecordFailure("truncated", key: roomKey, room: roomA, id: recordID, epoch: 1, sealed: rec1.prefix(rec1.count - 1))
addRecordFailure("too_short", key: roomKey, room: roomA, id: recordID, epoch: 1, sealed: rec1.prefix(27))

let secretHashes: [[String: Any]] = [inviteSecret, Data(count: 32)].map {
  ["invite_secret": hex($0), "hash": hex(sha256($0))]
}

var links: [[String: Any]] = []
func addLink(_ name: String, _ l: Link, encodes: Bool = true, prefix: String = "https://compassbrowser.org/join#") {
  links.append(["name": name, "json": l.json, "link": fragmentLink(l.json, prefix: prefix),
                "encodes": encodes, "expected": l.expected])
}
let base = Link(s: "https://sync.example.com", r: roomA, i: "inv_4f2a-9c", x: inviteSecret, k: roomKey,
                e: 1, n: "Reading list", t: "folder")
addLink("folder", base)
var l2 = base
l2.s = "https://rooms.example.org:8443/compass/v2"; l2.t = "space"; l2.e = 0xffff_ffff
l2.n = "Équipe \"Nord\" \\ ✨\nTab\t\u{1}"; l2.r = roomB; l2.k = otherKey; l2.i = "A"
addLink("space_unicode_escapes", l2)
var l3 = base
l3.s = "http://localhost:8295"; l3.t = "easel"; l3.e = 0; l3.n = ""; l3.i = String(repeating: "z", count: 64)
addLink("easel_localhost", l3)
var l4 = base
l4.s = "http://127.0.0.1:8295/"; l4.t = "easel"
addLink("loopback_ipv4", l4)
var l5 = base
l5.s = "http://[::1]:8295"
addLink("loopback_ipv6", l5)
addLink("deep_link", base, encodes: false, prefix: "compass://join#")
// Unknown keys are ignored; key order doesn't matter when reading.
let reordered = "{\"t\":\"folder\",\"s\":\"https://sync.example.com\",\"x\":\"\(b64url(inviteSecret))\",\"v\":2,"
  + "\"r\":\"\(b64url(roomA))\",\"n\":\"Reading list\",\"k\":\"\(b64url(roomKey))\",\"i\":\"inv_4f2a-9c\",\"e\":1}"
links.append(["name": "unknown_keys_and_order", "json": reordered, "link": fragmentLink(reordered),
              "encodes": false, "expected": base.expected])

var invalid: [[String: Any]] = []
func bad(_ name: String, _ link: String) { invalid.append(["name": name, "link": link]) }
func badJSON(_ name: String, _ json: String) { bad(name, fragmentLink(json)) }
func badLink(_ name: String, _ change: (inout Link) -> Void) {
  var l = base
  change(&l)
  badJSON(name, l.json)
}
let good = fragmentLink(base.json)
let goodFragment = String(good.dropFirst("https://compassbrowser.org/join#".count))
bad("empty", "")
bad("no_fragment", "https://compassbrowser.org/join")
bad("empty_fragment", "https://compassbrowser.org/join#")
bad("other_host", "https://example.com/join#" + goodFragment)
bad("http_site", "http://compassbrowser.org/join#" + goodFragment)
bad("other_path", "https://compassbrowser.org/boost#" + goodFragment)
bad("query_before_fragment", "https://compassbrowser.org/join?x=1#" + goodFragment)
bad("other_deep_link", "compass://boost#" + goodFragment)
bad("padded", good + "=")
bad("standard_base64", "https://compassbrowser.org/join#" + Data(base.json.utf8).base64EncodedString())
bad("not_base64", "https://compassbrowser.org/join#!!!!")
bad("trailing_space", good + " ")
bad("too_long", "https://compassbrowser.org/join#" + String(repeating: "A", count: 4096))
badJSON("not_json", "not json")
badJSON("array", "[]")
badJSON("missing_server", base.json.replacingOccurrences(of: ",\"s\":\"https://sync.example.com\"", with: ""))
badJSON("missing_epoch", base.json.replacingOccurrences(of: "\"e\":1,", with: ""))
badJSON("epoch_negative", base.json.replacingOccurrences(of: "\"e\":1,", with: "\"e\":-1,"))
badJSON("epoch_too_big", base.json.replacingOccurrences(of: "\"e\":1,", with: "\"e\":4294967296,"))
badJSON("epoch_fraction", base.json.replacingOccurrences(of: "\"e\":1,", with: "\"e\":1.5,"))
badJSON("epoch_string", base.json.replacingOccurrences(of: "\"e\":1,", with: "\"e\":\"1\","))
badJSON("epoch_bool", base.json.replacingOccurrences(of: "\"e\":1,", with: "\"e\":true,"))
badJSON("name_not_string", base.json.replacingOccurrences(of: "\"n\":\"Reading list\"", with: "\"n\":5"))
badLink("unknown_kind") { $0.t = "tab" }
badLink("kind_capitalized") { $0.t = "Folder" }
badLink("room_id_short") { $0.r = roomA.prefix(15) }
badLink("room_id_long") { $0.r = roomA + Data([0]) }
badLink("secret_short") { $0.x = inviteSecret.prefix(31) }
badLink("key_long") { $0.k = roomKey + Data([0]) }
badJSON("room_id_padded", base.json.replacingOccurrences(of: b64url(roomA), with: b64url(roomA) + "=="))
badJSON("room_id_noncanonical", base.json.replacingOccurrences(of: b64url(roomA), with: String(b64url(roomA).dropLast()) + "_"))
badLink("invite_id_empty") { $0.i = "" }
badLink("invite_id_too_long") { $0.i = String(repeating: "z", count: 65) }
badLink("invite_id_bad_chars") { $0.i = "inv/1" }
badLink("name_too_long") { $0.n = String(repeating: "é", count: 129) }
badLink("server_http_remote") { $0.s = "http://sync.example.com" }
badLink("server_ftp") { $0.s = "ftp://sync.example.com" }
badLink("server_not_url") { $0.s = "sync.example.com" }
badLink("server_userinfo") { $0.s = "https://user:pw@sync.example.com" }
badLink("server_query") { $0.s = "https://sync.example.com/?a=b" }
badLink("server_fragment") { $0.s = "https://sync.example.com/#x" }
badLink("server_space") { $0.s = "https://sync.example.com/a b" }
badLink("server_uppercase_scheme") { $0.s = "HTTPS://sync.example.com" }
badLink("server_no_host") { $0.s = "https:///path" }
badLink("server_backslash") { $0.s = "https://sync.example.com\\evil.example" }
badLink("server_bad_port") { $0.s = "https://sync.example.com:99999" }
badLink("server_port_not_number") { $0.s = "https://sync.example.com:80a" }
badLink("server_empty_port") { $0.s = "https://sync.example.com:/" }
badLink("server_localhost_uppercase_http") { $0.s = "http://LOCALHOST:8295" }
badLink("server_localhost_suffix_http") { $0.s = "http://localhost.example.com" }

// The longest valid name still fits.
precondition(fragmentLink({ var l = base; l.n = String(repeating: "é", count: 128); return l }().json).utf8.count <= 4096)

let root: [String: Any] = [
  "comment": "Rooms crypto and invite links (docs/design/sync.md, \"Rooms: crypto and links\"). "
    + "Made by common/rooms/make_vectors.swift; all binary values are lowercase hex.",
  "identity": identity,
  "envelopes": envelopes,
  "envelope_failures": envelopeFailures,
  "records": records,
  "record_failures": recordFailures,
  "invite_secret_hashes": secretHashes,
  "links": links,
  "invalid_links": invalid,
]
let out = try! JSONSerialization.data(withJSONObject: root, options: [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes])
FileHandle.standardOutput.write(out + Data("\n".utf8))
