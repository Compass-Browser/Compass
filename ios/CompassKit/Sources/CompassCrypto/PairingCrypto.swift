// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CCryptoBoringSSL
import CryptoKit
import Foundation

// The cryptographic half of pairing, ported from the desktop's
// components/sync/pairing.cc (docs/design/sync.md, "Pairing API"):
//
//   1. B -> A: B's SPAKE2 message (32 bytes).
//   2. A -> B: A's SPAKE2 message (32 bytes).
//   3. B -> A: MAC_b (32) || nonce (12) || AES-256-GCM(K_enc, device name,
//      ad="compass-pair-b").
//   4. A -> B: MAC_a (32) || verdict (0x01 allow, 0x00 deny) [allow only:
//      || nonce (12) || AES-256-GCM(K_enc, {"seed": base64, "profiles": []},
//      ad="compass-pair-a")].

public enum PairingRole: Sendable {
  case a, b

  var other: PairingRole { self == .a ? .b : .a }
  public var name: String { self == .a ? "a" : "b" }
}

/// "7-482913": the slot number, a dash, then 6 digits (the secret, which
/// never reaches the server).
public struct PairingCode: Sendable, Equatable {
  public static let digitCount = 6
  public let slot: Int
  public let digits: String

  public init?(_ text: String) {
    let code = text.trimmingCharacters(in: .whitespacesAndNewlines)
      .replacingOccurrences(of: " ", with: "")
    guard let dash = code.firstIndex(of: "-"), dash != code.startIndex else { return nil }
    let slotText = code[..<dash]
    let digits = code[code.index(after: dash)...]
    guard slotText.allSatisfy(\.isASCIIDigit), let slot = Int(slotText), slot >= 1,
      digits.count == Self.digitCount, digits.allSatisfy(\.isASCIIDigit)
    else { return nil }
    self.slot = slot
    self.digits = String(digits)
  }

  public init(slot: Int, digits: String) {
    self.slot = slot
    self.digits = digits
  }

  public var text: String { "\(slot)-\(digits)" }

  /// A uniformly random 6-digit secret.
  public static func randomDigits() -> String {
    let range: UInt32 = 1_000_000
    let limit = (UInt32.max / range) * range
    var value: UInt32
    repeat {
      value = UInt32.random(in: 0...UInt32.max)
    } while value >= limit
    let digits = String(value % range)
    return String(repeating: "0", count: digitCount - digits.count) + digits
  }
}

/// What the desktop's QR code holds:
/// `compass-pair:?server=<sync server URL>&code=<slot>-<digits>`. The
/// app registers the scheme, so the system camera opens Compass too.
public struct PairingInvite: Sendable, Equatable {
  public static let scheme = "compass-pair"
  public let server: URL
  public let code: PairingCode

  public init(server: URL, code: PairingCode) {
    self.server = server
    self.code = code
  }

  public init?(url: URL) {
    guard url.scheme == Self.scheme,
      let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems,
      let serverText = items.first(where: { $0.name == "server" })?.value,
      let server = URL(string: serverText), server.scheme == "https" || server.scheme == "http",
      let codeText = items.first(where: { $0.name == "code" })?.value,
      let code = PairingCode(codeText)
    else { return nil }
    self.init(server: server, code: code)
  }

  public var url: URL {
    var c = URLComponents()
    c.scheme = Self.scheme
    c.queryItems = [
      URLQueryItem(name: "server", value: server.absoluteString),
      URLQueryItem(name: "code", value: code.text),
    ]
    return c.url!
  }
}

public struct PairingKeys: Sendable {
  public let enc: SymmetricKey
  public let confirmA: SymmetricKey
  public let confirmB: SymmetricKey

  /// K_enc, K_confirm_a, K_confirm_b = HKDF-SHA256(SPAKE2 key, salt "",
  /// info "compass-pair-enc" / "-confirm-a" / "-confirm-b").
  public init(spake2Key: Data) {
    func derive(_ info: String) -> SymmetricKey {
      HKDF<SHA256>.deriveKey(
        inputKeyMaterial: SymmetricKey(data: spake2Key), salt: Data(),
        info: Data(info.utf8), outputByteCount: 32)
    }
    enc = derive("compass-pair-enc")
    confirmA = derive("compass-pair-confirm-a")
    confirmB = derive("compass-pair-confirm-b")
  }

  /// MAC_a or MAC_b, by who sends it.
  public func confirmMac(sender: PairingRole) -> Data {
    let (key, label) =
      sender == .a
      ? (confirmA, "compass-pair-confirm-a") : (confirmB, "compass-pair-confirm-b")
    return Data(HMAC<SHA256>.authenticationCode(for: Data(label.utf8), using: key))
  }

  public func verifyMac(_ mac: Data, sender: PairingRole) -> Bool {
    let (key, label) =
      sender == .a
      ? (confirmA, "compass-pair-confirm-a") : (confirmB, "compass-pair-confirm-b")
    return mac.count == 32
      && HMAC<SHA256>.isValidAuthenticationCode(mac, authenticating: Data(label.utf8), using: key)
  }

  static let adFromA = Data("compass-pair-a".utf8)
  static let adFromB = Data("compass-pair-b".utf8)

  /// nonce (12) || ciphertext || tag (16).
  public func seal(_ plaintext: Data, from sender: PairingRole) -> Data {
    let box = try! AES.GCM.seal(
      plaintext, using: enc, authenticating: sender == .a ? Self.adFromA : Self.adFromB)
    return box.combined!
  }

  public func open(_ sealed: Data, from sender: PairingRole) -> Data? {
    guard sealed.count >= 12 + 16, let box = try? AES.GCM.SealedBox(combined: sealed) else {
      return nil
    }
    return try? AES.GCM.open(box, using: enc, authenticating: sender == .a ? Self.adFromA : Self.adFromB)
  }
}

/// SPAKE2 names: "compass-a:" + host + ":" + slot, "compass-b:...".
public func pairingSpakeName(_ role: PairingRole, host: String, slot: Int) -> String {
  "compass-\(role.name):\(host):\(slot)"
}

/// BoringSSL's SPAKE2 over edwards25519, as the desktop uses it.
public final class Spake2 {
  public static let messageSize = 32
  private let ctx: OpaquePointer

  public init?(role: PairingRole, host: String, slot: Int) {
    let mine = Array(pairingSpakeName(role, host: host, slot: slot).utf8)
    let theirs = Array(pairingSpakeName(role.other, host: host, slot: slot).utf8)
    guard
      let ctx = CCryptoBoringSSL_SPAKE2_CTX_new(
        role == .a ? spake2_role_alice : spake2_role_bob, mine, mine.count, theirs, theirs.count)
    else { return nil }
    self.ctx = ctx
  }

  deinit { CCryptoBoringSSL_SPAKE2_CTX_free(ctx) }

  /// Our message for the 6 digits; call once.
  public func generateMessage(password: String) -> Data? {
    var out = [UInt8](repeating: 0, count: Int(SPAKE2_MAX_MSG_SIZE))
    var length = 0
    let pw = Array(password.utf8)
    guard CCryptoBoringSSL_SPAKE2_generate_msg(ctx, &out, &length, out.count, pw, pw.count) == 1
    else { return nil }
    return Data(out.prefix(length))
  }

  /// The pairing keys from the peer's message, or nil.
  public func processMessage(_ theirs: Data) -> PairingKeys? {
    guard theirs.count == Self.messageSize else { return nil }
    var key = [UInt8](repeating: 0, count: Int(SPAKE2_MAX_KEY_SIZE))
    var length = 0
    let msg = [UInt8](theirs)
    guard CCryptoBoringSSL_SPAKE2_process_msg(ctx, &key, &length, key.count, msg, msg.count) == 1
    else { return nil }
    return PairingKeys(spake2Key: Data(key.prefix(length)))
  }
}

extension Character {
  var isASCIIDigit: Bool { isASCII && isNumber }
}
