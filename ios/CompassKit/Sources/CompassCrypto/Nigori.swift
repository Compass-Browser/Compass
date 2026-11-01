// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CCryptoBoringSSL
import CommonCrypto
import CryptoKit
import Foundation

/// One Nigori key: Chromium's sync encryption (components/sync/model/crypto/
/// nigori.cc). AES-128-CBC (PKCS#7) with a random IV, then HMAC-SHA256;
/// the wire form is base64(iv || ciphertext || mac).
public struct Nigori: Sendable, Equatable {
  public static let keySize = 16
  static let ivSize = 16
  static let hashSize = 32

  public let encryptionKey: Data
  public let macKey: Data
  /// Kept only to export keys as Chromium does (16 zero bytes for scrypt).
  public let userKey: Data?

  public enum Derivation: Sendable, Equatable {
    /// PBKDF2-HMAC-SHA1, Chromium's pre-M70 custom passphrases.
    case pbkdf2
    /// scrypt N=8192, r=8, p=11 with the Nigori's salt (raw bytes).
    case scrypt(salt: Data)
  }

  public init?(encryptionKey: Data, macKey: Data, userKey: Data? = nil) {
    guard encryptionKey.count == Self.keySize, macKey.count == Self.keySize else { return nil }
    self.encryptionKey = encryptionKey
    self.macKey = macKey
    self.userKey = userKey?.count == Self.keySize ? userKey : nil
  }

  /// The key a custom passphrase derives (Nigori::CreateByDerivation).
  public init(passphrase: String, derivation: Derivation) {
    let password = Data(passphrase.utf8)
    switch derivation {
    case .pbkdf2:
      let salt = Data([
        0xc7, 0xca, 0xfb, 0x23, 0xec, 0x2a, 0x9d, 0x4c,
        0x03, 0x5a, 0x90, 0xae, 0xed, 0x8b, 0xa4, 0x98,
      ])
      self.init(
        encryptionKey: Self.pbkdf2(password, salt, rounds: 1003),
        macKey: Self.pbkdf2(password, salt, rounds: 1004),
        userKey: Self.pbkdf2(password, salt, rounds: 1002))!
    case .scrypt(let salt):
      let keys = Self.scrypt(password, salt, length: 2 * Self.keySize)
      self.init(
        encryptionKey: keys.prefix(Self.keySize), macKey: keys.suffix(Self.keySize),
        userKey: Data(count: Self.keySize))!
    }
  }

  /// Permute[Kenc,Kmac](Nigori::Password || "nigori-key"): the key's ID in
  /// keybags and EncryptedData.key_name.
  public var keyName: String {
    var plaintext = Data()
    // NigoriStream: a type is BE32(4) BE32(type), a string BE32(len) bytes.
    plaintext.appendBigEndian(UInt32(4))
    plaintext.appendBigEndian(UInt32(1))  // Nigori::Password
    let name = Data("nigori-key".utf8)
    plaintext.appendBigEndian(UInt32(name.count))
    plaintext.append(name)
    let ciphertext = Self.aesCBC(encrypt: true, key: encryptionKey, iv: Data(count: Self.ivSize), plaintext)!
    let mac = HMAC<SHA256>.authenticationCode(for: ciphertext, using: SymmetricKey(data: macKey))
    return (ciphertext + Data(mac)).base64EncodedString()
  }

  /// Enc[Kenc,Kmac](value), base64. The MAC covers the ciphertext only, as
  /// Chromium writes with kSyncNigoriAuthenticateIV off (its default).
  public func encrypt(_ plaintext: Data) -> String {
    var iv = [UInt8](repeating: 0, count: Self.ivSize)
    precondition(SecRandomCopyBytes(kSecRandomDefault, iv.count, &iv) == errSecSuccess)
    let ciphertext = Self.aesCBC(encrypt: true, key: encryptionKey, iv: Data(iv), plaintext)!
    let mac = HMAC<SHA256>.authenticationCode(for: ciphertext, using: SymmetricKey(data: macKey))
    return (Data(iv) + ciphertext + Data(mac)).base64EncodedString()
  }

  /// Accepts a MAC over iv || ciphertext or over the ciphertext alone.
  public func decrypt(_ encrypted: String) -> Data? {
    guard let input = Data(base64Encoded: encrypted),
      input.count >= Self.ivSize * 2 + Self.hashSize
    else { return nil }
    let iv = input.prefix(Self.ivSize)
    let ciphertext = input.dropFirst(Self.ivSize).dropLast(Self.hashSize)
    let mac = input.suffix(Self.hashSize)
    let key = SymmetricKey(data: macKey)
    guard
      HMAC<SHA256>.isValidAuthenticationCode(mac, authenticating: input.dropLast(Self.hashSize), using: key)
        || HMAC<SHA256>.isValidAuthenticationCode(mac, authenticating: ciphertext, using: key)
    else { return nil }
    return Self.aesCBC(encrypt: false, key: encryptionKey, iv: Data(iv), Data(ciphertext))
  }

  // MARK: Primitives

  static func pbkdf2(_ password: Data, _ salt: Data, rounds: UInt32) -> Data {
    var out = [UInt8](repeating: 0, count: keySize)
    let status = password.withUnsafeBytes { p in
      salt.withUnsafeBytes { s in
        CCKeyDerivationPBKDF(
          CCPBKDFAlgorithm(kCCPBKDF2), p.bindMemory(to: CChar.self).baseAddress, p.count,
          s.bindMemory(to: UInt8.self).baseAddress, s.count, CCPseudoRandomAlgorithm(kCCPRFHmacAlgSHA1),
          rounds, &out, out.count)
      }
    }
    precondition(status == kCCSuccess)
    return Data(out)
  }

  static func scrypt(_ password: Data, _ salt: Data, length: Int) -> Data {
    var out = [UInt8](repeating: 0, count: length)
    let ok = password.withUnsafeBytes { p in
      salt.withUnsafeBytes { s in
        CCryptoBoringSSL_EVP_PBE_scrypt(
          p.bindMemory(to: CChar.self).baseAddress, p.count,
          s.bindMemory(to: UInt8.self).baseAddress, s.count,
          8192, 8, 11, 32 * 1024 * 1024, &out, out.count)
      }
    }
    precondition(ok == 1)
    return Data(out)
  }

  static func aesCBC(encrypt: Bool, key: Data, iv: Data, _ input: Data) -> Data? {
    var out = [UInt8](repeating: 0, count: input.count + kCCBlockSizeAES128)
    var moved = 0
    let status = key.withUnsafeBytes { k in
      iv.withUnsafeBytes { v in
        input.withUnsafeBytes { i in
          CCCrypt(
            CCOperation(encrypt ? kCCEncrypt : kCCDecrypt), CCAlgorithm(kCCAlgorithmAES),
            CCOptions(kCCOptionPKCS7Padding), k.baseAddress, k.count, v.baseAddress,
            i.baseAddress, i.count, &out, out.count, &moved)
        }
      }
    }
    return status == kCCSuccess ? Data(out.prefix(moved)) : nil
  }
}

extension Data {
  mutating func appendBigEndian(_ value: UInt32) {
    Swift.withUnsafeBytes(of: value.bigEndian) { append(contentsOf: $0) }
  }
}
