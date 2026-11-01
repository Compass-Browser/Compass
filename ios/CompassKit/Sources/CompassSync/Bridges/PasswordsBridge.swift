// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Security
import SwiftProtobuf
import SyncProto

/// A saved password, from its PasswordSpecificsData.
public struct SavedPassword: Sendable, Equatable, Identifiable {
  public var id: String { recordID }
  /// Chromium's client tag (see `PasswordsBridge.clientTag(for:)`).
  public var tag: String
  public var data: SyncPb_PasswordSpecificsData
  /// The profile account it's in: empty for the home profile's, else the
  /// profile's UUID (each profile has its own passwords, as on the desktop).
  public var profile = ""
  /// Unique across profiles (also the AutoFill record identifier).
  public var recordID: String { profile.isEmpty ? tag : profile + " " + tag }

  public var signonRealm: String { data.signonRealm }
  public var username: String { data.usernameValue }
  public var password: String { data.passwordValue }
  public var isBlocked: Bool { data.blacklisted }
  /// The host to show and match AutoFill against.
  public var host: String {
    URL(string: data.origin)?.host() ?? URL(string: data.signonRealm)?.host() ?? data.signonRealm
  }

  public init(data: SyncPb_PasswordSpecificsData) {
    self.data = data
    tag = PasswordsBridge.clientTag(data)
  }
}

/// Where passwords live on the device.
public protocol PasswordStore: AnyObject, Sendable {
  func all() -> [SavedPassword]
  func put(_ password: SavedPassword)
  func delete(tag: String)
}

public final class MemoryPasswordStore: PasswordStore, @unchecked Sendable {
  private let lock = NSLock()
  private var items: [String: SavedPassword] = [:]
  public init() {}
  public func all() -> [SavedPassword] { lock.withLock { Array(items.values) } }
  public func put(_ password: SavedPassword) { lock.withLock { items[password.tag] = password } }
  public func delete(tag: String) { _ = lock.withLock { items.removeValue(forKey: tag) } }
}

/// Passwords in the Keychain, one generic-password item per credential,
/// in an access group the AutoFill extension shares. The item's data is the
/// serialized PasswordSpecificsData, so fields this client doesn't know
/// survive.
public final class KeychainPasswordStore: PasswordStore, @unchecked Sendable {
  static let baseService = "it.cipi1965.compass.passwords"
  let service: String
  let accessGroup: String?
  let profile: String

  /// `profile`: empty for the home profile's passwords, else its UUID
  /// (Keychain service "it.cipi1965.compass.passwords.<uuid>").
  public init(accessGroup: String?, profile: String = "") {
    self.accessGroup = accessGroup
    self.profile = profile
    service = profile.isEmpty ? Self.baseService : Self.baseService + "." + profile
  }

  static let namesService = "it.cipi1965.compass.profile-names"

  /// The chain's profile names by account ("" = the home's), kept in the
  /// shared Keychain so the AutoFill extension can head its groups.
  public static func saveProfileNames(_ names: [String: String], accessGroup: String?) {
    guard let data = try? JSONEncoder().encode(names) else { return }
    var q: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: namesService,
      kSecAttrAccount as String: "names",
    ]
    if let accessGroup { q[kSecAttrAccessGroup as String] = accessGroup }
    let update: [String: Any] = [kSecValueData as String: data]
    if SecItemUpdate(q as CFDictionary, update as CFDictionary) == errSecItemNotFound {
      q.merge(update) { $1 }
      q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
      SecItemAdd(q as CFDictionary, nil)
    }
  }

  public static func profileNames(accessGroup: String?) -> [String: String] {
    var q: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: namesService,
      kSecAttrAccount as String: "names", kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne,
    ]
    if let accessGroup { q[kSecAttrAccessGroup as String] = accessGroup }
    var result: CFTypeRef?
    guard SecItemCopyMatching(q as CFDictionary, &result) == errSecSuccess, let data = result as? Data else { return [:] }
    return (try? JSONDecoder().decode([String: String].self, from: data)) ?? [:]
  }

  /// Every profile's passwords (the AutoFill extension and QuickType list
  /// cover them all).
  public static func everyProfile(accessGroup: String?) -> [SavedPassword] {
    var q: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword, kSecReturnData as String: true,
      kSecReturnAttributes as String: true, kSecMatchLimit as String: kSecMatchLimitAll,
    ]
    if let accessGroup { q[kSecAttrAccessGroup as String] = accessGroup }
    var result: CFTypeRef?
    guard SecItemCopyMatching(q as CFDictionary, &result) == errSecSuccess, let items = result as? [[String: Any]]
    else { return [] }
    return items.compactMap { item in
      guard let service = item[kSecAttrService as String] as? String,
        service == baseService || service.hasPrefix(baseService + "."),
        let data = item[kSecValueData as String] as? Data,
        let d = try? SyncPb_PasswordSpecificsData(serializedBytes: data)
      else { return nil }
      var p = SavedPassword(data: d)
      p.profile = service == baseService ? "" : String(service.dropFirst(baseService.count + 1))
      return p
    }
  }

  private func base() -> [String: Any] {
    var q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service]
    if let accessGroup { q[kSecAttrAccessGroup as String] = accessGroup }
    return q
  }

  public func all() -> [SavedPassword] {
    var q = base()
    q[kSecReturnData as String] = true
    q[kSecMatchLimit as String] = kSecMatchLimitAll
    var result: CFTypeRef?
    guard SecItemCopyMatching(q as CFDictionary, &result) == errSecSuccess, let items = result as? [Data] else {
      return []
    }
    return items.compactMap { try? SyncPb_PasswordSpecificsData(serializedBytes: $0) }.map {
      var p = SavedPassword(data: $0)
      p.profile = profile
      return p
    }
  }

  public func put(_ password: SavedPassword) {
    guard let data = try? password.data.serializedBytes() as Data else { return }
    var q = base()
    q[kSecAttrAccount as String] = password.tag
    let update: [String: Any] = [kSecValueData as String: data, kSecAttrLabel as String: password.host]
    if SecItemUpdate(q as CFDictionary, update as CFDictionary) == errSecItemNotFound {
      q.merge(update) { $1 }
      q[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
      SecItemAdd(q as CFDictionary, nil)
    }
  }

  public func delete(tag: String) {
    var q = base()
    q[kSecAttrAccount as String] = tag
    SecItemDelete(q as CFDictionary)
  }
}

/// PASSWORDS, as Chromium's PasswordSyncBridge. Client tag =
/// EscapePath(origin spec) | EscapePath(username element) |
/// EscapePath(username) | EscapePath(password element) |
/// EscapePath(signon realm) (password_proto_utils.cc, GetClientTag).
public final class PasswordsBridge: DataTypeBridge, @unchecked Sendable {
  public nonisolated let dataType = DataType.passwords
  let store: any PasswordStore
  let engine: SyncEngine
  /// After any change (the app refreshes the AutoFill identity store).
  public var onChange: (@Sendable ([SavedPassword]) -> Void)?

  public init(store: any PasswordStore, engine: SyncEngine) {
    self.store = store
    self.engine = engine
  }

  public nonisolated func clientTag(for specifics: SyncPb_EntitySpecifics) -> String {
    Self.clientTag(specifics.password.clientOnlyEncryptedData)
  }

  public static func clientTag(_ d: SyncPb_PasswordSpecificsData) -> String {
    [canonicalOrigin(d.origin), d.usernameElement, d.usernameValue, d.passwordElement, d.signonRealm]
      .map(escapePath).joined(separator: "|")
  }

  /// GURL's spec for a typical origin: lowercase scheme and host, "/" for
  /// an empty path; "" if it isn't a valid URL.
  static func canonicalOrigin(_ s: String) -> String {
    guard var c = URLComponents(string: s), let scheme = c.scheme, c.host != nil else { return "" }
    c.scheme = scheme.lowercased()
    c.host = c.host?.lowercased()
    if c.path.isEmpty { c.path = "/" }
    if (scheme == "https" && c.port == 443) || (scheme == "http" && c.port == 80) { c.port = nil }
    return c.string ?? ""
  }

  /// base::EscapePath: kPathCharmap's characters as uppercase %XX.
  static func escapePath(_ s: String) -> String {
    let escaped: Set<UInt8> = Set(Array(0...0x20) + [0x22, 0x23, 0x25, 0x3a, 0x3c, 0x3e, 0x3f, 0x5b, 0x5c, 0x5d, 0x5e, 0x60, 0x7b, 0x7c, 0x7d] + Array(0x7f...0xff))
    var out = ""
    for byte in s.utf8 {
      if escaped.contains(byte) {
        out += String(format: "%%%02X", byte)
      } else {
        out.unicodeScalars.append(UnicodeScalar(byte))
      }
    }
    return out
  }

  public func apply(_ changes: [RemoteChange]) async {
    let byHash = Dictionary(
      store.all().map { (ClientTagHash.make(type: .passwords, tag: $0.tag), $0.tag) }, uniquingKeysWith: { a, _ in a })
    for change in changes {
      if let specifics = change.specifics {
        store.put(SavedPassword(data: specifics.password.clientOnlyEncryptedData))
      } else if let tag = byHash[change.clientTagHash] {
        store.delete(tag: tag)
      }
    }
    onChange?(store.all())
  }

  public func localDataForInitialMerge() async -> [(tag: String, specifics: SyncPb_EntitySpecifics)] {
    store.all().map { ($0.tag, Self.specifics($0)) }
  }

  static func specifics(_ p: SavedPassword) -> SyncPb_EntitySpecifics {
    var s = SyncPb_EntitySpecifics()
    s.password.clientOnlyEncryptedData = p.data
    return s
  }

  // MARK: Local changes

  /// The credential to store for a password typed into a page: a new one,
  /// or the existing one with the new password (fields kept).
  public static func makeSaved(
    existing: [SavedPassword], origin: String, signonRealm: String, username: String, password: String,
    usernameElement: String = "", passwordElement: String = "", action: String = "", now: Date = Date()
  ) -> SavedPassword {
    let micros = Int64(now.timeIntervalSince1970 * 1_000_000) + 11_644_473_600_000_000
    var d = SyncPb_PasswordSpecificsData()
    d.scheme = 0  // HTML form
    d.signonRealm = signonRealm
    d.origin = canonicalOrigin(origin)
    if !action.isEmpty { d.action = action }
    d.usernameElement = usernameElement
    d.usernameValue = username
    d.passwordElement = passwordElement
    d.passwordValue = password
    d.dateCreated = micros
    d.datePasswordModifiedWindowsEpochMicros = micros
    d.dateLastUsed = micros
    d.type = 0  // manually saved
    d.timesUsed = 1
    let tag = clientTag(d)
    if let found = existing.first(where: { $0.tag == tag }) {
      var kept = found.data
      kept.passwordValue = password
      kept.datePasswordModifiedWindowsEpochMicros = micros
      kept.dateLastUsed = micros
      kept.blacklisted = false
      d = kept
    }
    return SavedPassword(data: d)
  }

  /// Saves (or updates) a password typed into a page.
  @discardableResult
  public func save(
    origin: String, signonRealm: String, username: String, password: String, usernameElement: String = "",
    passwordElement: String = "", action: String = "", now: Date = Date()
  ) async -> SavedPassword {
    let saved = Self.makeSaved(
      existing: store.all(), origin: origin, signonRealm: signonRealm, username: username, password: password,
      usernameElement: usernameElement, passwordElement: passwordElement, action: action, now: now)
    store.put(saved)
    await engine.put(.passwords, tag: saved.tag, specifics: Self.specifics(saved))
    onChange?(store.all())
    return saved
  }

  /// "Never for this site": Chromium's blocklisted entry (no username or
  /// password, blacklisted = true).
  public func neverSave(origin: String, signonRealm: String) async {
    var d = SyncPb_PasswordSpecificsData()
    d.signonRealm = signonRealm
    d.origin = Self.canonicalOrigin(origin)
    d.blacklisted = true
    d.dateCreated = Int64(Date().timeIntervalSince1970 * 1_000_000) + 11_644_473_600_000_000
    let saved = SavedPassword(data: d)
    store.put(saved)
    await engine.put(.passwords, tag: saved.tag, specifics: Self.specifics(saved))
  }

  public func delete(_ password: SavedPassword) async {
    store.delete(tag: password.tag)
    await engine.delete(.passwords, tag: password.tag)
    onChange?(store.all())
  }

  /// Saved logins for a page: same signon realm (scheme://host/), or a
  /// subdomain's realm under the same registrable host (Chromium's PSL match,
  /// simplified to "same host or a parent of it").
  public static func matches(_ all: [SavedPassword], for pageURL: URL) -> [SavedPassword] {
    guard let host = pageURL.host()?.lowercased(), let scheme = pageURL.scheme?.lowercased() else { return [] }
    let realm = "\(scheme)://\(host)/"
    return all.filter { p in
      guard !p.isBlocked else { return false }
      if p.signonRealm == realm { return true }
      guard let h = URL(string: p.signonRealm)?.host()?.lowercased() else { return false }
      return host.hasSuffix("." + h) || h.hasSuffix("." + host)
    }
  }

  public static func isBlocked(_ all: [SavedPassword], for pageURL: URL) -> Bool {
    guard let host = pageURL.host()?.lowercased(), let scheme = pageURL.scheme?.lowercased() else { return false }
    return all.contains { $0.isBlocked && $0.signonRealm == "\(scheme)://\(host)/" }
  }
}
