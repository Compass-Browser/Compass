// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation
import SwiftProtobuf
import SyncProto

public enum CryptographerError: Error, Equatable {
  /// The chain's Nigori isn't a custom passphrase one (every Compass chain is).
  case notCustomPassphrase(Int32)
  case missingKeybag
  /// The passphrase doesn't open the keybag.
  case wrongPassphrase
  /// The keybag opened but isn't what Chromium writes.
  case malformedKeybag
}

/// The chain's keys, from its NIGORI entity and the custom passphrase
/// (HKDF(seed, "compass-sync-passphrase")). Mirrors Chromium's
/// NigoriSyncBridgeImpl + CryptographerImpl for the custom-passphrase case.
public struct Cryptographer: Sendable {
  public private(set) var keys: [String: Nigori]
  public private(set) var defaultKeyName: String

  public init(keys: [Nigori], defaultKeyName: String) {
    self.keys = Dictionary(keys.map { ($0.keyName, $0) }, uniquingKeysWith: { a, _ in a })
    self.defaultKeyName = defaultKeyName
  }

  /// Opens the keybag of a downloaded NIGORI with the passphrase.
  public init(nigori: SyncPb_NigoriSpecifics, passphrase: String) throws {
    guard nigori.passphraseType == 4 else {  // CUSTOM_PASSPHRASE
      throw CryptographerError.notCustomPassphrase(nigori.passphraseType)
    }
    guard nigori.hasEncryptionKeybag, !nigori.encryptionKeybag.blob.isEmpty else {
      throw CryptographerError.missingKeybag
    }
    let derivation: Nigori.Derivation
    switch nigori.customPassphraseKeyDerivationMethod {
    case 2:  // SCRYPT_8192_8_11
      guard let salt = Data(base64Encoded: nigori.customPassphraseKeyDerivationSalt) else {
        throw CryptographerError.malformedKeybag
      }
      derivation = .scrypt(salt: salt)
    default:  // UNSPECIFIED and PBKDF2_HMAC_SHA1_1003
      derivation = .pbkdf2
    }
    let passphraseKey = Nigori(passphrase: passphrase, derivation: derivation)
    let keybag = nigori.encryptionKeybag
    guard keybag.keyName == passphraseKey.keyName,
      let plaintext = passphraseKey.decrypt(keybag.blob)
    else { throw CryptographerError.wrongPassphrase }
    guard let decoded = try? SyncPb_EncryptionKeys(serializedBytes: plaintext) else {
      throw CryptographerError.malformedKeybag
    }
    let keys = decoded.key.compactMap {
      Nigori(encryptionKey: $0.encryptionKey, macKey: $0.macKey, userKey: $0.deprecatedUserKey)
    }
    self.init(keys: keys, defaultKeyName: keybag.keyName)
    guard self.keys[defaultKeyName] != nil else { throw CryptographerError.malformedKeybag }
  }

  /// A new chain's NIGORI (the first device): one scrypt key from the
  /// passphrase, which is also the keybag's key, everything encrypted.
  public static func makeCustomPassphraseNigori(passphrase: String, now: Date = Date())
    -> (SyncPb_NigoriSpecifics, Cryptographer)
  {
    var salt = [UInt8](repeating: 0, count: 32)
    precondition(SecRandomCopyBytes(kSecRandomDefault, salt.count, &salt) == errSecSuccess)
    let key = Nigori(passphrase: passphrase, derivation: .scrypt(salt: Data(salt)))
    let cryptographer = Cryptographer(keys: [key], defaultKeyName: key.keyName)

    var keys = SyncPb_EncryptionKeys()
    var proto = SyncPb_NigoriKey()
    proto.deprecatedName = key.keyName
    proto.deprecatedUserKey = key.userKey ?? Data(count: Nigori.keySize)
    proto.encryptionKey = key.encryptionKey
    proto.macKey = key.macKey
    keys.key = [proto]

    var nigori = SyncPb_NigoriSpecifics()
    nigori.encryptionKeybag = cryptographer.encrypt(try! keys.serializedBytes())
    nigori.keybagIsFrozen = true
    nigori.encryptEverything = true
    nigori.passphraseType = 4
    nigori.customPassphraseKeyDerivationMethod = 2
    nigori.customPassphraseKeyDerivationSalt = Data(salt).base64EncodedString()
    nigori.customPassphraseTime = Int64(now.timeIntervalSince1970 * 1000)
    return (nigori, cryptographer)
  }

  public func canDecrypt(_ data: SyncPb_EncryptedData) -> Bool { keys[data.keyName] != nil }

  public func decrypt(_ data: SyncPb_EncryptedData) -> Data? {
    keys[data.keyName]?.decrypt(data.blob)
  }

  public func encrypt(_ plaintext: Data) -> SyncPb_EncryptedData {
    var out = SyncPb_EncryptedData()
    out.keyName = defaultKeyName
    out.blob = keys[defaultKeyName]!.encrypt(plaintext)
    return out
  }

  /// Specifics as the server holds them: `encrypted` set, plus the type's
  /// own (empty) field so the server knows the type.
  public func encrypt(specifics: SyncPb_EntitySpecifics, type: DataType) -> SyncPb_EntitySpecifics {
    var out = type.emptySpecifics
    out.encrypted = encrypt(try! specifics.serializedBytes())
    return out
  }

  /// The real specifics of a downloaded entity, or nil if its key is unknown.
  public func decrypt(specifics: SyncPb_EntitySpecifics) -> SyncPb_EntitySpecifics? {
    guard specifics.hasEncrypted else { return specifics }
    guard let plaintext = decrypt(specifics.encrypted) else { return nil }
    return try? SyncPb_EntitySpecifics(serializedBytes: plaintext)
  }
}
