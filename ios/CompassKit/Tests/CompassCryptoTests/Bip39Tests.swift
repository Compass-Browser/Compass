// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation
import Testing

// The desktop's bip39_unittest.cc, ported.
struct Bip39Tests {
  // The BIP39 reference implementation's 128-bit English vectors, plus our
  // derivation test seed.
  static let vectors: [(String, String)] = [
    ("00000000000000000000000000000000",
     "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"),
    ("7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f",
     "legal winner thank year wave sausage worth useful legal winner thank yellow"),
    ("80808080808080808080808080808080",
     "letter advice cage absurd amount doctor acoustic avoid letter advice cage above"),
    ("ffffffffffffffffffffffffffffffff", "zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong"),
    ("9e885d952ad362caeb4efe34a8e91bd2",
     "ozone drill grab fiber curtain grace pudding thank cruise elder eight picnic"),
    ("c0ba5a8e914111210f2bd131f3d5e08d",
     "scheme spot photo card baby mountain device kick cradle pact join borrow"),
    ("000102030405060708090a0b0c0d0e0f",
     "abandon amount liar amount expire adjust cage candy arch gather drum buyer"),
  ]

  @Test func wordlist() {
    let words = Bip39.words
    #expect(words.count == 2048)
    #expect(words.first == "abandon")
    #expect(words.last == "zoo")
    #expect(words == words.sorted())
    #expect(Bip39.index(of: "about") == 3)
    #expect(Bip39.index(of: "zoo") == 2047)
    #expect(Bip39.index(of: "compass") == nil)
    #expect(Bip39.index(of: "") == nil)
  }

  @Test func entropyToWords() {
    for (hex, words) in Self.vectors {
      #expect(Bip39.words(fromEntropy: Data(hex: hex)!).joined(separator: " ") == words)
    }
  }

  @Test func wordsToEntropy() {
    for (hex, words) in Self.vectors {
      #expect(Bip39.entropy(fromWords: words.components(separatedBy: " ")) == Data(hex: hex)!)
    }
  }

  @Test func checksumIsChecked() {
    // The last word carries the checksum.
    #expect(Bip39.parse(
      "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon")
      == nil)
    #expect(Bip39.parse("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo") == nil)
  }

  @Test func rejectsWrongLengthAndUnknownWords() {
    #expect(Bip39.parse("abandon about") == nil)
    #expect(Bip39.parse(
      "compass abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about")
      == nil)
  }

  @Test func parseAcceptsTyping() {
    #expect(Bip39.parse(
      "  Legal winner\tthank year wave sausage\nworth useful legal winner thank YELLOW ")
      == Data(hex: "7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f7f"))
    #expect(Bip39.parse("") == nil)
  }

  @Test func wordsWithPrefix() {
    #expect(Bip39.words(withPrefix: "abs", max: 10)
      == ["absent", "absorb", "abstract", "absurd"])
    #expect(Bip39.words(withPrefix: "abs", max: 2) == ["absent", "absorb"])
    #expect(Bip39.words(withPrefix: "zoo", max: 10) == ["zoo"])
    #expect(Bip39.words(withPrefix: "qq", max: 10).isEmpty)
  }
}
