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

  /// A fresh random seed, for "Reset sync" or a chain started on the phone.
  public static func generateSeed() -> Data {
    var bytes = [UInt8](repeating: 0, count: seedLength)
    precondition(SecRandomCopyBytes(kSecRandomDefault, seedLength, &bytes) == errSecSuccess)
    return Data(bytes)
  }

  public init(recoveryCode: String) throws {
    guard let seed = Bip39.parse(recoveryCode) else { throw ChainError.badRecoveryCode }
    try self.init(seed: seed)
  }

  public var recoveryWords: [String] { Bip39.words(fromEntropy: seed) }

  /// The auth key of a profile's own server account (sync.md, "Profiles");
  /// an empty UUID gives the chain's home account, `authKey`.
  public func authKey(profileUUID: String) -> Data {
    profileUUID.isEmpty
      ? authKey : Chain.hkdf(seed, info: "compass-sync-auth:" + profileUUID)
  }

  /// The Nigori passphrase of a profile's own account: base64 of
  /// HKDF-SHA256(seed, "compass-sync-passphrase:" + uuid), per profile so
  /// one account's data doesn't decrypt in another (sync.md, "Slice 5
  /// notes"); an empty UUID gives the chain's home account, `passphrase`.
  public func passphrase(profileUUID: String) -> String {
    profileUUID.isEmpty
      ? passphrase : Chain.hkdf(seed, info: "compass-sync-passphrase:" + profileUUID).base64EncodedString()
  }

  /// The bearer token for one request, in go-sync's format:
  /// base64url_padded(HEX(ts) "|" HEX(ed25519(ts)) "|" HEX(pubkey)), where
  /// ts is the decimal Unix time in milliseconds and HEX is uppercase.
  /// The server accepts it for ±24 h around `now`.
  public func accessToken(now: Date = Date(), profileUUID: String = "") -> String {
    Chain.accessToken(authKey: authKey(profileUUID: profileUUID), now: now)
  }

  public static func accessToken(authKey: Data, now: Date) -> String {
    let timestamp = String(Int64((now.timeIntervalSince1970 * 1000).rounded(.down)))
    let message = Data(timestamp.utf8)
    let signature = Ed25519.sign(message, seed: authKey)
    let plain = message.hexString(uppercase: true) + "|"
      + signature.hexString(uppercase: true) + "|"
      + Ed25519.publicKey(fromSeed: authKey).hexString(uppercase: true)
    return Data(plain.utf8).base64URLEncodedString(padded: true)
  }

  static func hkdf(_ seed: Data, info: String) -> Data {
    let key = HKDF<SHA256>.deriveKey(
      inputKeyMaterial: SymmetricKey(data: seed), salt: Data(),
      info: Data(info.utf8), outputByteCount: 32)
    return key.withUnsafeBytes { Data($0) }
  }
}

public enum ChainError: Error, Equatable {
  case badSeedLength(Int)
  case badRecoveryCode
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

  public static func sign(_ message: Data, seed: Data) -> Data {
    precondition(seed.count == 32)
    var pub = [UInt8](repeating: 0, count: 32)
    var priv = [UInt8](repeating: 0, count: 64)
    seed.withUnsafeBytes { s in
      CCryptoBoringSSL_ED25519_keypair_from_seed(
        &pub, &priv, s.bindMemory(to: UInt8.self).baseAddress)
    }
    var signature = [UInt8](repeating: 0, count: 64)
    let ok = message.withUnsafeBytes { m in
      CCryptoBoringSSL_ED25519_sign(
        &signature, m.bindMemory(to: UInt8.self).baseAddress, m.count, priv)
    }
    precondition(ok == 1)
    return Data(signature)
  }
}

extension Data {
  public func base64URLEncodedString(padded: Bool) -> String {
    var s = base64EncodedString()
      .replacingOccurrences(of: "+", with: "-")
      .replacingOccurrences(of: "/", with: "_")
    if !padded { s = s.trimmingCharacters(in: CharacterSet(charactersIn: "=")) }
    return s
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
