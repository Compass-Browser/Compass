// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CryptoKit
import Foundation
import Testing

struct PairingCryptoTests {
  // The desktop's PairingKeysTest.Vectors (pairing_unittest.cc).
  @Test func keyVectors() {
    let keys = PairingKeys(spake2Key: Data((0..<64).map { UInt8($0) }))
    func hex(_ key: SymmetricKey) -> String {
      key.withUnsafeBytes { Data($0) }.hexString(uppercase: false)
    }
    #expect(hex(keys.enc) == "5144144d90ed02f3805d0a59510e4a0c7d7454adf0f4cda9e20390497a857653")
    #expect(hex(keys.confirmA) == "d71f8461386ed03eb405b0d5fa82478a93b636c943b35260c2e9af7863ef6a2a")
    #expect(hex(keys.confirmB) == "46d4fef69618607a51b39229754d45363e452737e49b53c98009c6b7dd61a05a")
    #expect(keys.confirmMac(sender: .a).hexString(uppercase: false)
      == "5640ef8734b1be15e7d60a12bac07ba7583b237137037eafc44bff2d7f3366f8")
    #expect(keys.confirmMac(sender: .b).hexString(uppercase: false)
      == "d0ccb754ea5fd62bed000484f428a444a674852d9725b1e65c7aa62a12ff0ac4")
    #expect(keys.verifyMac(keys.confirmMac(sender: .a), sender: .a))
    #expect(!keys.verifyMac(keys.confirmMac(sender: .b), sender: .a))
    #expect(pairingSpakeName(.a, host: "sync.example.com", slot: 7) == "compass-a:sync.example.com:7")
    #expect(pairingSpakeName(.b, host: "sync.example.com", slot: 7) == "compass-b:sync.example.com:7")
  }

  @Test func spakeAgreesOnlyWithTheSameCode() throws {
    func run(_ a: String, _ b: String) throws -> (PairingKeys, PairingKeys) {
      let alice = try #require(Spake2(role: .a, host: "h", slot: 3))
      let bob = try #require(Spake2(role: .b, host: "h", slot: 3))
      let ma = try #require(alice.generateMessage(password: a))
      let mb = try #require(bob.generateMessage(password: b))
      #expect(ma.count == 32)
      return (try #require(alice.processMessage(mb)), try #require(bob.processMessage(ma)))
    }
    let (ka, kb) = try run("482913", "482913")
    #expect(kb.verifyMac(ka.confirmMac(sender: .a), sender: .a))
    let (wa, wb) = try run("482913", "482914")
    #expect(!wb.verifyMac(wa.confirmMac(sender: .a), sender: .a))
  }

  @Test func sealOpensOnlyWithTheRightDirection() {
    let keys = PairingKeys(spake2Key: Data(repeating: 7, count: 64))
    let sealed = keys.seal(Data("iPhone".utf8), from: .b)
    #expect(keys.open(sealed, from: .b) == Data("iPhone".utf8))
    #expect(keys.open(sealed, from: .a) == nil)
    #expect(keys.open(Data([1, 2, 3]), from: .b) == nil)
  }

  @Test func parsesCodes() {
    #expect(PairingCode(" 7-482913 ") == PairingCode(slot: 7, digits: "482913"))
    #expect(PairingCode("12 - 000001") == PairingCode(slot: 12, digits: "000001"))
    for bad in ["", "7482913", "-482913", "0-482913", "7-48291", "7-4829134", "x-482913",
                "7-48291a", "+7-482913"] {
      #expect(PairingCode(bad) == nil, "\(bad)")
    }
    #expect(PairingCode(slot: 7, digits: "482913").text == "7-482913")
  }

  @Test func randomDigitsAreSixDigits() {
    for _ in 0..<100 {
      let d = PairingCode.randomDigits()
      #expect(d.count == 6 && d.allSatisfy(\.isNumber))
    }
  }

  @Test func inviteRoundTrips() throws {
    let invite = PairingInvite(
      server: URL(string: "https://sync.example.org/v2")!,
      code: PairingCode(slot: 7, digits: "482913"))
    #expect(invite.url.absoluteString
      == "compass-pair:?server=https://sync.example.org/v2&code=7-482913")
    #expect(PairingInvite(url: invite.url) == invite)
    #expect(PairingInvite(url: URL(string: "compass-pair:?server=ftp://x&code=7-482913")!) == nil)
    #expect(PairingInvite(url: URL(string: "https://x/?server=https://x&code=7-482913")!) == nil)
  }

  /// The desktop's QR (CompassSyncPageHandler::PairingInviteUrl) escapes the
  /// server URL; it must parse to the same invite.
  @Test func parsesTheDesktopsEscapedInvite() throws {
    let url = try #require(URL(string:
      "compass-pair:?server=https%3A%2F%2Fsync.example.org%2Fv2&code=7-482913"))
    let invite = try #require(PairingInvite(url: url))
    #expect(invite.server.absoluteString == "https://sync.example.org/v2")
    #expect(invite.code.text == "7-482913")
    let port = try #require(PairingInvite(url: URL(string:
      "compass-pair:?server=http%3A%2F%2Fh%3A8295%2Fv2&code=12-000001")!))
    #expect(port.server.absoluteString == "http://h:8295/v2")
  }
}
