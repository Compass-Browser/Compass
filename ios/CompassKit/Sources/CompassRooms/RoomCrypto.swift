// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CryptoKit
import Foundation

/// Rooms' cryptography (shared spaces, folders and easels): the identity key,
/// room key envelopes and record encryption, exactly as the desktop's
/// components/sync/rooms/room_crypto.cc. See docs/design/sync.md, "Rooms:
/// crypto and links"; both test common/rooms/vectors.json.
public enum RoomCrypto {
  public static let roomIDLength = 16
  public static let roomKeyLength = 32
  public static let inviteSecretLength = 32
  public static let x25519KeyLength = 32
  /// HPKE enc (32) || the sealed room key (32 + 16 tag).
  public static let envelopeLength = 32 + roomKeyLength + 16
  public static let recordNonceLength = 12
  /// nonce || ciphertext || tag: this much more than the plaintext.
  public static let recordOverhead = recordNonceLength + 16

  public static let identityInfo = "compass-identity-x25519"
  public static let envelopeInfoPrefix = "compass-room-key"
  public static let recordAADPrefix = "compass-room-record"

  /// HPKE (RFC 9180) base mode with DHKEM(X25519, HKDF-SHA256), HKDF-SHA256
  /// and AES-256-GCM. CryptoKit has no preset for that combination.
  public static let ciphersuite = HPKE.Ciphersuite(
    kem: .Curve25519_HKDF_SHA256, kdf: .HKDF_SHA256, aead: .AES_GCM_256)

  // MARK: Identity

  /// HKDF-SHA256(seed, salt "", info "compass-identity-x25519"[":" + uuid])
  /// as the X25519 private key. An empty UUID gives the chain's home
  /// account's key, as `Chain.authKey(profileUUID:)`.
  public static func identityKey(seed: Data, profileUUID: String = "") -> IdentityKey {
    let info = profileUUID.isEmpty ? identityInfo : identityInfo + ":" + profileUUID
    let key = HKDF<SHA256>.deriveKey(
      inputKeyMaterial: SymmetricKey(data: seed), salt: Data(), info: Data(info.utf8),
      outputByteCount: x25519KeyLength)
    // 32 bytes always make a key.
    return try! IdentityKey(privateKey: key.withUnsafeBytes { Data($0) })
  }

  public static func identityKey(chain: Chain, profileUUID: String = "") -> IdentityKey {
    identityKey(seed: chain.seed, profileUUID: profileUUID)
  }

  // MARK: Random values

  public static func generateRoomID() -> Data { random(roomIDLength) }
  public static func generateRoomKey() -> Data { random(roomKeyLength) }
  public static func generateInviteSecret() -> Data { random(inviteSecretLength) }

  /// What the server stores for a link invite: SHA-256(secret), lowercase
  /// hex. The secret itself stays in the link.
  public static func inviteSecretHash(_ secret: Data) -> String {
    Data(SHA256.hash(data: secret)).hexString(uppercase: false)
  }

  // MARK: Envelopes

  /// "compass-room-key" || room ID || epoch (big-endian UInt32).
  public static func envelopeInfo(roomID: Data, epoch: UInt32) -> Data {
    Data(envelopeInfoPrefix.utf8) + roomID + bigEndian(epoch)
  }

  /// Seals `roomKey` to a member's identity public key. Returns
  /// enc || ciphertext (`envelopeLength` bytes). The ephemeral key is random,
  /// so two envelopes of the same key differ.
  public static func sealEnvelope(
    recipientPublicKey: Data, roomID: Data, epoch: UInt32, roomKey: Data
  ) throws -> Data {
    guard roomID.count == roomIDLength, roomKey.count == roomKeyLength else {
      throw RoomCryptoError.badLength
    }
    var sender = try HPKE.Sender(
      recipientKey: Curve25519.KeyAgreement.PublicKey(rawRepresentation: recipientPublicKey),
      ciphersuite: ciphersuite, info: envelopeInfo(roomID: roomID, epoch: epoch))
    return sender.encapsulatedKey + (try sender.seal(roomKey))
  }

  /// The room key in `envelope`, or nil if it isn't for `identity`, this
  /// room and this epoch, or was tampered with.
  public static func openEnvelope(
    identity: IdentityKey, roomID: Data, epoch: UInt32, envelope: Data
  ) -> Data? {
    guard envelope.count == envelopeLength, roomID.count == roomIDLength,
      let privateKey = try? Curve25519.KeyAgreement.PrivateKey(rawRepresentation: identity.privateKey),
      var recipient = try? HPKE.Recipient(
        privateKey: privateKey, ciphersuite: ciphersuite,
        info: envelopeInfo(roomID: roomID, epoch: epoch),
        encapsulatedKey: Data(envelope.prefix(x25519KeyLength))),
      let key = try? recipient.open(Data(envelope.dropFirst(x25519KeyLength))),
      key.count == roomKeyLength
    else { return nil }
    return key
  }

  // MARK: Records

  /// "compass-room-record" || room ID || record ID (UTF-8) || epoch
  /// (big-endian UInt32).
  public static func recordAAD(roomID: Data, recordID: String, epoch: UInt32) -> Data {
    Data(recordAADPrefix.utf8) + roomID + Data(recordID.utf8) + bigEndian(epoch)
  }

  /// AES-256-GCM of `plaintext` (a serialized CompassSpacesSpecifics) with a
  /// random nonce. Returns nonce || ciphertext || tag.
  public static func encryptRecord(
    roomKey: Data, roomID: Data, recordID: String, epoch: UInt32, plaintext: Data
  ) throws -> Data {
    try encryptRecord(
      roomKey: roomKey, roomID: roomID, recordID: recordID, epoch: epoch, plaintext: plaintext,
      nonce: random(recordNonceLength))
  }

  /// As above with the nonce given: for the vectors only.
  static func encryptRecord(
    roomKey: Data, roomID: Data, recordID: String, epoch: UInt32, plaintext: Data, nonce: Data
  ) throws -> Data {
    guard roomKey.count == roomKeyLength, roomID.count == roomIDLength,
      nonce.count == recordNonceLength
    else { throw RoomCryptoError.badLength }
    let box = try AES.GCM.seal(
      plaintext, using: SymmetricKey(data: roomKey), nonce: AES.GCM.Nonce(data: nonce),
      authenticating: recordAAD(roomID: roomID, recordID: recordID, epoch: epoch))
    return nonce + box.ciphertext + box.tag
  }

  /// The plaintext of a record, or nil if the key, room, record ID or epoch
  /// don't match or it was tampered with.
  public static func decryptRecord(
    roomKey: Data, roomID: Data, recordID: String, epoch: UInt32, sealed: Data
  ) -> Data? {
    guard roomKey.count == roomKeyLength, roomID.count == roomIDLength,
      sealed.count >= recordOverhead,
      let box = try? AES.GCM.SealedBox(combined: sealed)
    else { return nil }
    return try? AES.GCM.open(
      box, using: SymmetricKey(data: roomKey),
      authenticating: recordAAD(roomID: roomID, recordID: recordID, epoch: epoch))
  }

  // MARK: Helpers

  static func bigEndian(_ value: UInt32) -> Data {
    withUnsafeBytes(of: value.bigEndian) { Data($0) }
  }

  static func random(_ count: Int) -> Data {
    var bytes = [UInt8](repeating: 0, count: count)
    precondition(SecRandomCopyBytes(kSecRandomDefault, count, &bytes) == errSecSuccess)
    return Data(bytes)
  }
}

/// An account's X25519 key pair. Members publish `publicKey`; envelopes for
/// them are sealed to it.
public struct IdentityKey: Sendable, Equatable {
  /// The private scalar as derived (X25519 clamps it when used).
  public let privateKey: Data
  public let publicKey: Data

  public init(privateKey: Data) throws {
    guard privateKey.count == RoomCrypto.x25519KeyLength else { throw RoomCryptoError.badLength }
    self.privateKey = privateKey
    publicKey = try Curve25519.KeyAgreement.PrivateKey(rawRepresentation: privateKey)
      .publicKey.rawRepresentation
  }
}

public enum RoomCryptoError: Error, Equatable {
  case badLength
}
