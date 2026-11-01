// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import SyncProto
import Testing

struct CryptographerTests {
  let passphrase = "xoFg1Bm10gjM8Ahji5ibCeSkV6zo7kZEcjBFP9M8s50="

  @Test func opensTheNigoriItMade() throws {
    let (nigori, made) = Cryptographer.makeCustomPassphraseNigori(passphrase: passphrase)
    let opened = try Cryptographer(nigori: nigori, passphrase: passphrase)
    #expect(opened.defaultKeyName == made.defaultKeyName)
    #expect(opened.keys.count == 1)

    var password = SyncPb_EntitySpecifics()
    password.compassSpaces.space.name = "Work"
    let encrypted = made.encrypt(specifics: password, type: .compassSpaces)
    #expect(encrypted.hasEncrypted)
    #expect(encrypted.specificsVariant == .compassSpaces(SyncPb_CompassSpacesSpecifics()))
    #expect(opened.decrypt(specifics: encrypted)?.compassSpaces.space.name == "Work")
  }

  @Test func wrongPassphrase() {
    let (nigori, _) = Cryptographer.makeCustomPassphraseNigori(passphrase: passphrase)
    #expect(throws: CryptographerError.wrongPassphrase) {
      try Cryptographer(nigori: nigori, passphrase: "nope")
    }
  }

  @Test func keystoreNigoriIsRejected() {
    var nigori = SyncPb_NigoriSpecifics()
    nigori.passphraseType = 2
    #expect(throws: CryptographerError.notCustomPassphrase(2)) {
      try Cryptographer(nigori: nigori, passphrase: passphrase)
    }
  }

  // A keybag holding several keys (after a passphrase change on another
  // device): old data stays readable, new data uses the default key.
  @Test func keepsOlderKeys() throws {
    let old = Nigori(passphrase: "old", derivation: .pbkdf2)
    let salt = Data(repeating: 1, count: 32)
    let current = Nigori(passphrase: passphrase, derivation: .scrypt(salt: salt))
    var keys = SyncPb_EncryptionKeys()
    keys.key = [old, current].map {
      var k = SyncPb_NigoriKey()
      k.encryptionKey = $0.encryptionKey
      k.macKey = $0.macKey
      return k
    }
    var nigori = SyncPb_NigoriSpecifics()
    nigori.passphraseType = 4
    nigori.customPassphraseKeyDerivationMethod = 2
    nigori.customPassphraseKeyDerivationSalt = salt.base64EncodedString()
    nigori.encryptionKeybag.keyName = current.keyName
    nigori.encryptionKeybag.blob = current.encrypt(try keys.serializedBytes())

    let c = try Cryptographer(nigori: nigori, passphrase: passphrase)
    var oldData = SyncPb_EncryptedData()
    oldData.keyName = old.keyName
    oldData.blob = old.encrypt(Data("old".utf8))
    #expect(c.decrypt(oldData) == Data("old".utf8))
    #expect(c.encrypt(Data("new".utf8)).keyName == current.keyName)
  }
}
