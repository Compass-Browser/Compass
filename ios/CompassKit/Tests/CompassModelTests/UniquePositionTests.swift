// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassModel
import CryptoKit
import Foundation
import Testing

// Chromium's unique_position_unittest.cc, ported where it pins behavior.
struct UniquePositionTests {
  /// The legacy Directory's suffixes, as Chromium's SuffixGenerator.
  struct SuffixGenerator {
    let guid: String
    var next: Int64 = -65535
    mutating func nextSuffix() -> [UInt8] {
      let input = guid + String(next)
      next -= 1
      return Array(Data(Insecure.SHA1.hash(data: Data(input.utf8))).base64EncodedString().utf8)
    }
  }

  @Test func oneClientInsertAtEnd() {
    var g = SuffixGenerator(guid: "tuiWdG8hV+8y4RT9N5Aikg==")
    var pos = UniquePosition.initial(suffix: g.nextSuffix())
    for _ in 0..<1024 {
      let next = UniquePosition.after(pos, suffix: g.nextSuffix())
      #expect(pos < next)
      pos = next
    }
    #expect(pos.compressed.count < 500)
  }

  @Test func twoClientsInsertAtEnd() {
    var g1 = SuffixGenerator(guid: "tuiWdG8hV+8y4RT9N5Aikg==")
    var g2 = SuffixGenerator(guid: "yaKb7zHtY06aue9a0vlZgw==")
    var pos = UniquePosition.initial(suffix: g1.nextSuffix())
    for i in 0..<1024 {
      let next = UniquePosition.after(pos, suffix: i % 2 == 0 ? g1.nextSuffix() : g2.nextSuffix())
      #expect(pos < next)
      pos = next
    }
    #expect(pos.compressed.count < 500)
  }

  static let minSuffix: [UInt8] = Array(repeating: 0, count: 27) + [1]
  static let maxSuffix: [UInt8] = Array(repeating: 0xff, count: 28)
  static let normalSuffix: [UInt8] = Array("CRMkNGkyNjY5MTQzMzA0OTE0OSEw".utf8)

  @Test(arguments: [minSuffix, maxSuffix, normalSuffix])
  func insertsAtStartEndAndBetween(suffix a: [UInt8]) {
    var b = a
    b[10] ^= 0xff
    var c = a
    c[10] ^= 0xf0
    // At the start.
    var pos = UniquePosition.initial(suffix: a)
    for i in 0..<1024 {
      let next = UniquePosition.before(pos, suffix: i % 2 == 0 ? b : a)
      #expect(next < pos)
      pos = next
    }
    // At the end.
    pos = UniquePosition.initial(suffix: a)
    for i in 0..<1024 {
      let next = UniquePosition.after(pos, suffix: i % 2 == 0 ? b : a)
      #expect(pos < next)
      pos = next
    }
    // Between, always just right of the left one.
    var right = UniquePosition.initial(suffix: a)
    let left = UniquePosition.before(right, suffix: c)
    for i in 0..<1024 {
      let mid = UniquePosition.between(left, right, suffix: i % 2 == 0 ? b : a)
      #expect(left < mid && mid < right)
      right = mid
    }
  }

  @Test func compressedOrdering() {
    let suffix: (UInt8) -> [UInt8] = { last in (0..<27).map { UInt8($0) } + [last] }
    let prefixes: [[UInt8]] = [
      [0, 0, 0, 0, 0xff, 0xff, 0xfe, 0xff, 1],  // 256 0x00s
      [0, 0, 0, 0, 0xff, 0xff, 0xff, 0xfb, 1],  // four 0x00s
      [0xff, 0xff, 0xff, 0xff, 0, 0, 0, 4, 1],  // four 0xFFs
      [0xff, 0xff, 0xff, 0xff, 0, 0, 1, 0, 1],  // 256 0xFFs
    ]
    let positions = zip(prefixes, [4, 3, 1, 2] as [UInt8]).map {
      UniquePosition(compressed: $0 + suffix($1))
    }
    let allValid = positions.allSatisfy(\.isValid)
    #expect(allValid)
    for i in 0..<(positions.count - 1) { #expect(positions[i] < positions[i + 1]) }
    #expect(UniquePosition(compressed: []).isValid == false)
  }

  @Test func compressionRoundTripsAndKeepsOrder() {
    var rng = SystemRandomNumberGenerator()
    var values: [[UInt8]] = []
    for _ in 0..<500 {
      let length = Int.random(in: 0..<20, using: &rng)
      // Runs of repeated bytes exercise the run-length blocks.
      var bytes: [UInt8] = []
      while bytes.count < length {
        let b = [0, 1, 0x7f, 0xfe, 0xff].randomElement(using: &rng)! as UInt8
        bytes += [UInt8](repeating: b, count: Int.random(in: 1..<10, using: &rng))
      }
      values.append(bytes + Self.normalSuffix)
    }
    for v in values {
      #expect(UniquePosition.uncompress(UniquePosition.compress(v)) == v)
    }
    for _ in 0..<2000 {
      let x = values.randomElement()!
      let y = values.randomElement()!
      let plain = x.lexicographicallyPrecedes(y)
      let compressed = UniquePosition.compress(x).lexicographicallyPrecedes(UniquePosition.compress(y))
      #expect(plain == compressed)
    }
  }

  @Test func fromInt64KeepsOrder() {
    let ints: [Int64] = [.min, -1000, -1, 0, 1, 1000, .max]
    let positions = ints.map { UniquePosition.fromInt64($0, suffix: Self.normalSuffix) }
    for i in 0..<(positions.count - 1) { #expect(positions[i] < positions[i + 1]) }
  }

  @Test func suffixFromClientTagHash() {
    let s = UniquePosition.suffix(clientTagHash: "iNFQtRFQb+IZcn1kKUJEZDDkLs4=")
    #expect(s.count == 28 && s.last != 0)
  }

  @Test func forInsert() {
    let a = UniquePosition.initial(suffix: Self.normalSuffix)
    let s2 = UniquePosition.randomSuffix()
    #expect(UniquePosition.forInsert(after: a, before: nil, suffix: s2) > a)
    #expect(UniquePosition.forInsert(after: nil, before: a, suffix: s2) < a)
    let b = UniquePosition.after(a, suffix: UniquePosition.randomSuffix())
    let mid = UniquePosition.forInsert(after: a, before: b, suffix: s2)
    #expect(a < mid && mid < b)
  }
}
