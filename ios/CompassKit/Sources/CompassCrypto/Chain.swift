// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CCryptoBoringSSL
import CryptoKit
import Foundation

/// A sync chain's secrets, derived from its 16-byte seed exactly as the
/// desktop does (docs/design/sync.md, "Identity: a sync chain, no accounts";
/// components/sync/chain.cc on sync-main).
public struct Chain: Sendable {
  public static let seedLength = 16

  public let seed: Data
  /// The Ed25519 private seed: HKDF-SHA256(seed, info "compass-sync-auth").
  public let authKey: Data
  /// The Ed25519 public key; its lowercase hex is the chain ID on the server.
  public let publicKey: Data
  /// Chromium's custom passphrase: base64 of
  /// HKDF-SHA256(seed, info "compass-sync-passphrase").
  public let passphrase: String

  public init(seed: Data) throws {
    guard seed.count == Chain.seedLength else {
      throw ChainError.badSeedLength(seed.count)
    }
    self.seed = seed
    authKey = Chain.hkdf(seed, info: "compass-sync-auth")
    publicKey = Ed25519.publicKey(fromSeed: authKey)
    passphrase = Chain.hkdf(seed, info: "compass-sync-passphrase").base64EncodedString()
  }

  public var chainID: String { publicKey.hexString(uppercase: false) }

  static func hkdf(_ seed: Data, info: String) -> Data {
    let key = HKDF<SHA256>.deriveKey(
      inputKeyMaterial: SymmetricKey(data: seed), salt: Data(),
      info: Data(info.utf8), outputByteCount: 32)
    return key.withUnsafeBytes { Data($0) }
  }
}

public enum ChainError: Error, Equatable {
  case badSeedLength(Int)
}

/// Deterministic Ed25519 (RFC 8032) through BoringSSL. CryptoKit's signatures
/// are randomized, which would still verify but couldn't be checked against
/// the desktop's vectors.
public enum Ed25519 {
  public static func publicKey(fromSeed seed: Data) -> Data {
    precondition(seed.count == 32)
    var pub = [UInt8](repeating: 0, count: 32)
    var priv = [UInt8](repeating: 0, count: 64)
    seed.withUnsafeBytes { s in
      CCryptoBoringSSL_ED25519_keypair_from_seed(
        &pub, &priv, s.bindMemory(to: UInt8.self).baseAddress)
    }
    return Data(pub)
  }
}

extension Data {
  public func hexString(uppercase: Bool) -> String {
    let format = uppercase ? "%02X" : "%02x"
    return map { String(format: format, $0) }.joined()
  }

  public init?(hex: String) {
    guard hex.count % 2 == 0 else { return nil }
    var bytes = [UInt8]()
    bytes.reserveCapacity(hex.count / 2)
    var index = hex.startIndex
    while index < hex.endIndex {
      let next = hex.index(index, offsetBy: 2)
      guard let byte = UInt8(hex[index..<next], radix: 16) else { return nil }
      bytes.append(byte)
      index = next
    }
    self.init(bytes)
  }
}
