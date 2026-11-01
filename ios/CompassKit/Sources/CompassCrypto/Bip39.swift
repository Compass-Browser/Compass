// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CryptoKit
import Foundation

/// The recovery code: a 16-byte seed as 12 BIP39 English words (128 bits of
/// entropy plus a 4-bit SHA-256 checksum). A port of the desktop's
/// components/sync/bip39.cc.
public enum Bip39 {
  public static let wordCount = 12
  static let bitsPerWord = 11
  static let entropyBits = Chain.seedLength * 8  // 128
  static let checksumBits = entropyBits / 32  // 4

  public static var words: [String] { bip39EnglishWords }

  public static func index(of word: String) -> Int? {
    let words = bip39EnglishWords
    var low = 0
    var high = words.count
    while low < high {
      let mid = (low + high) / 2
      if words[mid] < word { low = mid + 1 } else { high = mid }
    }
    return low < words.count && words[low] == word ? low : nil
  }

  public static func words(fromEntropy entropy: Data) -> [String] {
    precondition(entropy.count == Chain.seedLength)
    let bytes = [UInt8](entropy)
    let checksum = Self.checksum(bytes)
    func bit(_ i: Int) -> Int {
      let byte = i < entropyBits ? bytes[i / 8] : checksum
      return Int(byte >> (7 - UInt8(i % 8))) & 1
    }
    return (0..<wordCount).map { w in
      var index = 0
      for b in 0..<bitsPerWord { index = (index << 1) | bit(w * bitsPerWord + b) }
      return bip39EnglishWords[index]
    }
  }

  /// The entropy, or nil for a wrong count, an unknown word or a bad checksum.
  public static func entropy(fromWords words: [String]) -> Data? {
    guard words.count == wordCount else { return nil }
    var entropy = [UInt8](repeating: 0, count: Chain.seedLength)
    var checksum: UInt8 = 0
    for (w, word) in words.enumerated() {
      guard let index = index(of: word) else { return nil }
      for b in 0..<bitsPerWord where (index >> (bitsPerWord - 1 - b)) & 1 == 1 {
        let i = w * bitsPerWord + b
        if i < entropyBits {
          entropy[i / 8] |= 1 << (7 - UInt8(i % 8))
        } else {
          checksum |= 1 << (7 - UInt8(i - entropyBits))
        }
      }
    }
    return Self.checksum(entropy) == checksum ? Data(entropy) : nil
  }

  /// Typed text: any whitespace between words, any case.
  public static func parse(_ text: String) -> Data? {
    entropy(fromWords: text.lowercased().split(whereSeparator: \.isWhitespace).map(String.init))
  }

  /// Autocomplete for the recovery-code field.
  public static func words(withPrefix prefix: String, max: Int) -> [String] {
    guard !prefix.isEmpty else { return [] }
    let words = bip39EnglishWords
    var low = 0
    var high = words.count
    while low < high {
      let mid = (low + high) / 2
      if words[mid] < prefix { low = mid + 1 } else { high = mid }
    }
    var result: [String] = []
    while low < words.count, result.count < max, words[low].hasPrefix(prefix) {
      result.append(words[low])
      low += 1
    }
    return result
  }

  static func checksum(_ entropy: [UInt8]) -> UInt8 {
    let digest = Array(SHA256.hash(data: entropy))
    return digest[0] & UInt8(truncatingIfNeeded: 0xff << (8 - checksumBits))
  }
}
