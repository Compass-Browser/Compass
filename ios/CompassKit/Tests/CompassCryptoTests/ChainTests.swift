// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation
import Testing

// Vectors from docs/design/sync.md ("Derivations"), shared with the desktop's
// components/sync/chain_unittest.cc.
struct ChainTests {
  let chain = try! Chain(seed: Data(hex: "000102030405060708090a0b0c0d0e0f")!)

  @Test func authKey() {
    #expect(chain.authKey.hexString(uppercase: false)
      == "be6eabbf861e7b696f17ae6bd46b4bd745d1190e0658e720ca55fa65104139ad")
  }

  @Test func chainID() {
    #expect(chain.chainID
      == "696a94d1a2ebbabf8e5012c07df3c65c7c6beaaf8ccaf4a67e6cdd3da9743467")
  }

  @Test func passphrase() {
    #expect(chain.passphrase == "xoFg1Bm10gjM8Ahji5ibCeSkV6zo7kZEcjBFP9M8s50=")
  }

  @Test func profileAuthKey() {
    #expect(chain.authKey(profileUUID: "2b5c3e1a-0000-4000-8000-000000000001")
      .hexString(uppercase: false)
      == "1b28e57a6e3ca95bc1b4b9c30f10f8d554ef09eed00844b52048aea6d90f4cf4")
    #expect(chain.authKey(profileUUID: "") == chain.authKey)
  }

  // sync.md, "Identity" (ChainTest.ProfileAccountVectors).
  @Test func profileAccountVectors() {
    let uuid = "2b5c3e1a-0000-4000-8000-000000000001"
    #expect(Ed25519.publicKey(fromSeed: chain.authKey(profileUUID: uuid)).hexString(uppercase: false)
      == "f1fe31e2c21e376e8d3b2ef57560b8466b37486e5fa46cc60bb39f1ea2ec9fab")
    #expect(chain.passphrase(profileUUID: uuid) == "C89l8oqXaTa997+mU79acx+BGgUNf6YPtMoPU9ODyEc=")
    #expect(chain.passphrase(profileUUID: "") == chain.passphrase)
  }

  @Test func accessTokenVector() {
    #expect(chain.accessToken(now: Date(timeIntervalSince1970: 1_700_000_000))
      == "MzEzNzMwMzAzMDMwMzAzMDMwMzAzMDMwMzB8MkY1REY4REVCNDUwRUJBMTk5MTgxNjdGODZFQjVDOThBRkQxREYyRUI5MTUzQjBDOENGMDQ1RDhGQjZDODk3MERFMjhFOUI0Qzg0QzJCMjUzRDcxMzI4QTVCNUJENzcwNUE0QTQ1QjFCQkVERkJBRjM5MzcyQTA1RTZEOTZCMDV8Njk2QTk0RDFBMkVCQkFCRjhFNTAxMkMwN0RGM0M2NUM3QzZCRUFBRjhDQ0FGNEE2N0U2Q0REM0RBOTc0MzQ2Nw==")
  }

  @Test func accessTokenKeepsMilliseconds() throws {
    let token = chain.accessToken(now: Date(timeIntervalSince1970: 1_761_900_000.123))
    let plain = String(decoding: Data(base64Encoded: token
      .replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/"))!,
      as: UTF8.self)
    let parts = plain.split(separator: "|")
    #expect(parts.count == 3)
    #expect(String(decoding: Data(hex: String(parts[0]))!, as: UTF8.self) == "1761900000123")
  }

  @Test func recoveryCodeRoundTrip() throws {
    #expect(chain.recoveryWords.joined(separator: " ")
      == "abandon amount liar amount expire adjust cage candy arch gather drum buyer")
    let back = try Chain(recoveryCode: chain.recoveryWords.joined(separator: " "))
    #expect(back.seed == chain.seed)
    #expect(throws: ChainError.badRecoveryCode) { try Chain(recoveryCode: "abandon") }
  }

  @Test func generatedSeedsDiffer() {
    #expect(Chain.generateSeed() != Chain.generateSeed())
    #expect(Chain.generateSeed().count == 16)
  }

  @Test func rejectsShortSeed() {
    #expect(throws: ChainError.badSeedLength(15)) {
      try Chain(seed: Data(count: 15))
    }
  }
}
