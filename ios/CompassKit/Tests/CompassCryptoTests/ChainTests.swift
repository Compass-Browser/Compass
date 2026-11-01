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

  @Test func rejectsShortSeed() {
    #expect(throws: ChainError.badSeedLength(15)) {
      try Chain(seed: Data(count: 15))
    }
  }
}
