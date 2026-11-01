// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassCrypto
import Foundation
import Testing

// Chromium's components/sync/model/crypto/nigori_unittest.cc, ported.
struct NigoriTests {
  let pbkdf2 = Nigori(passphrase: "password", derivation: .pbkdf2)

  @Test func keyName() {
    #expect(pbkdf2.keyName
      == "ibGL7ymU0Si+eYCXGS6SBHPFT+JCYiB6GDOYqj6vIwEiWJ7RENSHxmIQ8Q3rXd/UnZUmFHYB+jSIbthQADXvrQ==")
  }

  @Test func decryptsGoldens() {
    let nonAuthenticatedIV =
      "NNYlnzaaLPXWXyzz8J+u4OKgLiKRBPu2GJdjHWk0m3ADZrJhnmer30Zgiy4Ulxlfh6fmS71k8rop+UvSJdL1k/fcNLJ1C6sY5Z86ijyl1Jo="
    let authenticatedIV =
      "WaACU5CT+wnttSXFQmXgVjv4W1AqKdYURasywlvTZwv2RYvrOs3nc507759fvB1tMi32oOU77PSw1qg3ySF8kdF9VYEknEX+eC8vJni9vaE="
    #expect(pbkdf2.decrypt(nonAuthenticatedIV) == Data("test, test, 1, 2, 3".utf8))
    #expect(pbkdf2.decrypt(authenticatedIV) == Data("test, test, 1, 2, 3".utf8))
  }

  @Test func encryptDecrypt() {
    for text in ["value", ""] {
      let a = pbkdf2.encrypt(Data(text.utf8))
      #expect(a != pbkdf2.encrypt(Data(text.utf8)))  // random IV
      #expect(pbkdf2.decrypt(a) == Data(text.utf8))
    }
  }

  @Test func corruptedCiphertextFails() {
    var bytes = [UInt8](Data(base64Encoded: pbkdf2.encrypt(Data("test".utf8)))!)
    bytes[Nigori.ivSize + 2] ^= 1
    #expect(pbkdf2.decrypt(Data(bytes).base64EncodedString()) == nil)
    #expect(pbkdf2.decrypt("not base64!") == nil)
  }

  @Test func pbkdf2Derivation() {
    let n = Nigori(passphrase: "hunter2", derivation: .pbkdf2)
    #expect(n.userKey?.hexString(uppercase: false) == "025599e143c4923d77f65b99d97019a3")
    #expect(n.encryptionKey.hexString(uppercase: false) == "4596bf346572497d92b2a0e2146d93c1")
    #expect(n.macKey.hexString(uppercase: false) == "2292ad9db96fe590b22a58db50f6f545")
  }

  @Test func scryptDerivation() {
    let n = Nigori(passphrase: "hunter2", derivation: .scrypt(salt: Data("alpensalz".utf8)))
    #expect(n.userKey?.hexString(uppercase: false) == "00000000000000000000000000000000")
    #expect(n.encryptionKey.hexString(uppercase: false) == "8aa735e0091339a5e51da3b3dd1b328a")
    #expect(n.macKey.hexString(uppercase: false) == "a7e73611968dfd2bca5b3382aed451ba")
  }

  @Test func importRejectsWrongSizes() {
    #expect(Nigori(encryptionKey: Data(count: 16), macKey: Data(count: 15)) == nil)
    let imported = Nigori(encryptionKey: pbkdf2.encryptionKey, macKey: pbkdf2.macKey)!
    #expect(imported.keyName == pbkdf2.keyName)
    #expect(imported.decrypt(pbkdf2.encrypt(Data("x".utf8))) == Data("x".utf8))
  }
}
