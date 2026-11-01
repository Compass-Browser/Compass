// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CryptoKit
import Foundation

/// Chromium's UniquePosition (components/sync/base/unique_position.cc),
/// ported byte for byte: sortable byte strings that end in a 28-byte suffix
/// unique to the entity, so concurrent inserts never collide. Kept in its
/// order-preserving compressed form (the proto's custom_compressed_v1), so
/// comparing two positions is comparing bytes.
public struct UniquePosition: Sendable, Hashable, Comparable {
  public static let suffixLength = 28
  static let maxUncompressedSize = 10 * 1024 * 1024

  /// custom_compressed_v1; empty = invalid.
  public let compressed: [UInt8]

  public var isValid: Bool { !compressed.isEmpty }

  /// From custom_compressed_v1 bytes (invalid if they don't decode to a
  /// valid position).
  public init(compressed: [UInt8]) {
    self.compressed = Self.isValidBytes(Self.uncompress(compressed)) ? compressed : []
  }

  /// From the proto's legacy uncompressed `value`.
  public init(uncompressedValue: [UInt8]) {
    self.compressed = Self.isValidBytes(uncompressedValue) ? Self.compress(uncompressedValue) : []
  }

  private init(validUncompressed bytes: [UInt8]) {
    compressed = Self.compress(bytes)
  }

  public static func < (lhs: UniquePosition, rhs: UniquePosition) -> Bool {
    lhs.compressed.lexicographicallyPrecedes(rhs.compressed)
  }

  // MARK: Suffixes

  /// base64(SHA1(client tag hash)): 28 ASCII bytes, as GenerateSuffix.
  public static func suffix(clientTagHash: String) -> [UInt8] {
    let digest = Data(Insecure.SHA1.hash(data: Data(clientTagHash.utf8)))
    return Array(digest.base64EncodedString().utf8)
  }

  public static func randomSuffix() -> [UInt8] {
    var s = [UInt8](repeating: 0, count: suffixLength)
    precondition(SecRandomCopyBytes(kSecRandomDefault, s.count, &s) == errSecSuccess)
    s[suffixLength - 1] = 0x7f
    return s
  }

  static func isValidSuffix(_ suffix: [UInt8]) -> Bool {
    suffix.count == suffixLength && suffix.last != 0
  }

  static func isValidBytes(_ bytes: [UInt8]) -> Bool {
    bytes.count >= suffixLength && bytes.last != 0
  }

  // MARK: Making positions

  public static func initial(suffix: [UInt8]) -> UniquePosition {
    precondition(isValidSuffix(suffix))
    return UniquePosition(validUncompressed: suffix)
  }

  public static func fromInt64(_ x: Int64, suffix: [UInt8]) -> UniquePosition {
    var y = UInt64(bitPattern: x) ^ 0x8000_0000_0000_0000
    var bytes = [UInt8](repeating: 0, count: 8)
    for i in stride(from: 7, through: 0, by: -1) {
      bytes[i] = UInt8(truncatingIfNeeded: y)
      y >>= 8
    }
    return UniquePosition(validUncompressed: bytes + suffix)
  }

  public static func before(_ x: UniquePosition, suffix: [UInt8]) -> UniquePosition {
    precondition(isValidSuffix(suffix) && x.isValid)
    return UniquePosition(
      validUncompressed: findSmallerWithSuffix(uncompress(x.compressed), suffix) + suffix)
  }

  public static func after(_ x: UniquePosition, suffix: [UInt8]) -> UniquePosition {
    precondition(isValidSuffix(suffix) && x.isValid)
    return UniquePosition(
      validUncompressed: findGreaterWithSuffix(uncompress(x.compressed), suffix) + suffix)
  }

  public static func between(_ before: UniquePosition, _ after: UniquePosition, suffix: [UInt8])
    -> UniquePosition
  {
    precondition(isValidSuffix(suffix) && before.isValid && after.isValid && before < after)
    return UniquePosition(
      validUncompressed: findBetweenWithSuffix(
        uncompress(before.compressed), uncompress(after.compressed), suffix) + suffix)
  }

  /// A position for an item inserted between two neighbours (either may be
  /// missing): the shared helper for every model insert.
  public static func forInsert(after previous: UniquePosition?, before next: UniquePosition?, suffix: [UInt8])
    -> UniquePosition
  {
    switch (previous?.isValid == true ? previous : nil, next?.isValid == true ? next : nil) {
    case (nil, nil): return initial(suffix: suffix)
    case (let p?, nil): return after(p, suffix: suffix)
    case (nil, let n?): return before(n, suffix: suffix)
    case (let p?, let n?):
      return p < n ? between(p, n, suffix: suffix) : after(p, suffix: suffix)
    }
  }

  // MARK: Chromium's algorithms

  static func less(_ a: ArraySlice<UInt8>, _ b: ArraySlice<UInt8>) -> Bool {
    a.lexicographicallyPrecedes(b)
  }

  static func findSmallerWithSuffix(_ reference: [UInt8], _ suffix: [UInt8]) -> [UInt8] {
    let refZeroes = reference.firstIndex { $0 != 0 }!
    let suffixZeroes = suffix.firstIndex { $0 != 0 }!
    if suffixZeroes > refZeroes { return [] }
    if less(suffix[suffixZeroes...], reference[refZeroes...]) {
      return [UInt8](repeating: 0, count: refZeroes - suffixZeroes)
    } else if suffixZeroes > 1 {
      return [UInt8](repeating: 0, count: refZeroes - suffixZeroes + 1)
    } else {
      return [UInt8](repeating: 0, count: refZeroes) + [reference[refZeroes] / 2]
    }
  }

  static func findGreaterWithSuffix(_ reference: [UInt8], _ suffix: [UInt8]) -> [UInt8] {
    let refFFs = reference.firstIndex { $0 != 0xff } ?? reference.count
    let suffixFFs = suffix.firstIndex { $0 != 0xff } ?? suffix.count
    if suffixFFs > refFFs { return [] }
    if less(reference[refFFs...], suffix[suffixFFs...]) {
      return [UInt8](repeating: 0xff, count: refFFs - suffixFFs)
    } else if suffixFFs > 1 {
      return [UInt8](repeating: 0xff, count: refFFs - suffixFFs + 1)
    } else {
      let digit = Int(reference[refFFs])
      let gt = UInt8(truncatingIfNeeded: digit + (255 - digit + 1) / 2)
      return [UInt8](repeating: 0xff, count: refFFs) + [gt]
    }
  }

  static func findBetweenWithSuffix(_ before: [UInt8], _ after: [UInt8], _ suffix: [UInt8]) -> [UInt8] {
    if less(before[...], suffix[...]) && less(suffix[...], after[...]) { return [] }
    var mid: [UInt8] = []
    var i = 0
    while i < min(before.count, after.count) {
      let a = Int(before[i])
      let b = Int(after[i])
      if b - a >= 2 {
        mid.append(UInt8(a + (b - a) / 2))
        return mid
      } else if a == b {
        mid.append(UInt8(a))
        if less(before[(i + 1)...], suffix[...]) && less(suffix[...], after[(i + 1)...]) {
          return mid
        }
      } else {
        var midA = mid
        midA.append(UInt8(a))
        midA += findGreaterWithSuffix(Array(before[(i + 1)...]), suffix)
        if after.count > i + 1 {
          var midB = mid
          midB.append(UInt8(b))
          midB += findSmallerWithSuffix(Array(after[(i + 1)...]), suffix)
          if midB.count < midA.count { return midB }
        }
        return midA
      }
      i += 1
    }
    return mid + findSmallerWithSuffix(Array(after[i...]), suffix)
  }

  // MARK: Compression (order-preserving run-length, see the C++ comment)

  static func isRepeatedCharPrefix(_ s: [UInt8], _ i: Int) -> Bool {
    s[i] == s[i + 1] && s[i] == s[i + 2] && s[i] == s[i + 3]
  }

  static func compress(_ s: [UInt8]) -> [UInt8] {
    var out: [UInt8] = []
    out.reserveCapacity(48)
    var i = 0
    while i < s.count {
      if i + 4 <= s.count && isRepeatedCharPrefix(s, i) {
        out += s[i..<(i + 4)]
        let digit = s[i]
        let runsUntil = s[(i + 4)...].firstIndex { $0 != digit }
        let runLength: Int
        let high: Bool
        if let end = runsUntil {
          runLength = end - i
          high = s[end] > digit
        } else {
          runLength = s.count - i
          high = false
        }
        let encoded = high ? UInt32.max - UInt32(runLength) : UInt32(runLength)
        out += [UInt8(encoded >> 24 & 0xff), UInt8(encoded >> 16 & 0xff), UInt8(encoded >> 8 & 0xff),
                UInt8(encoded & 0xff)]
        i += runLength
      } else {
        let length = min(8, s.count - i)
        out += s[i..<(i + length)]
        i += length
      }
    }
    return out
  }

  static func uncompress(_ s: [UInt8]) -> [UInt8] {
    if s.count > maxUncompressedSize { return [] }
    var out: [UInt8] = []
    var i = 0
    while i + 8 <= s.count {
      if isRepeatedCharPrefix(s, i) {
        let encoded = UInt32(s[i + 4]) << 24 | UInt32(s[i + 5]) << 16 | UInt32(s[i + 6]) << 8 | UInt32(s[i + 7])
        let length = encoded & 0x8000_0000 != 0 ? UInt32.max - encoded : encoded
        if out.count + Int(length) > maxUncompressedSize { return [] }
        out += [UInt8](repeating: s[i], count: Int(length))
      } else {
        out += s[i..<(i + 8)]
      }
      i += 8
    }
    out += s[i...]
    return out.count > maxUncompressedSize ? [] : out
  }

  /// The uncompressed bytes (debugging, tests).
  public var bytes: [UInt8] { Self.uncompress(compressed) }
}
