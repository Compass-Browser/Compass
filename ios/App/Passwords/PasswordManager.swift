// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AuthenticationServices
import CompassSync
import Foundation
import Observation
import WebKit

/// Passwords in the app: the shared Keychain store (also read by the
/// AutoFill extension), filling and saving on pages, and the system's
/// AutoFill identity list. Saves go through sync when it's on. Each profile
/// has its own passwords and account, as on the desktop (M8): a page fills
/// from and saves to its space's profile; the AutoFill list has them all.
@MainActor
@Observable
final class PasswordManager {
  /// The Keychain group the AutoFill extension shares (Info.plist
  /// CompassKeychainGroup = $(AppIdentifierPrefix)it.cipi1965.compass.shared).
  static let accessGroup: String? = {
    guard let group = Bundle.main.object(forInfoDictionaryKey: "CompassKeychainGroup") as? String,
      !group.hasPrefix("$")
    else { return nil }
    return group
  }()

  /// The access group the app is signed with, if any.
  let group: String?
  /// Per profile account ("" = the home's).
  private var stores: [String: any PasswordStore] = [:]
  /// Every profile's passwords.
  private(set) var all: [SavedPassword] = []
  /// Tabs whose page has a password field.
  private(set) var formTabs: Set<String> = []
  /// A login just typed into a page, waiting for "Save?".
  var pendingSave: PendingSave?
  private var bridges: [String: PasswordsBridge] = [:]

  func setBridge(_ bridge: PasswordsBridge, for profile: String) {
    bridge.onChange = { [weak self] _ in Task { @MainActor in self?.reload() } }
    bridges[profile] = bridge
  }

  func removeBridges() { bridges = [:] }

  /// A profile account's store.
  func store(_ profile: String) -> any PasswordStore {
    if let store = stores[profile] { return store }
    let store = KeychainPasswordStore(accessGroup: group, profile: profile)
    stores[profile] = store
    return store
  }

  struct PendingSave: Identifiable, Equatable {
    let id = UUID()
    let origin: String
    let realm: String
    let username: String
    let password: String
    let usernameElement: String
    let passwordElement: String
    let action: String
    let isUpdate: Bool
    /// The profile account of the page's space.
    let profile: String
  }

  init() {
    group = Self.usableGroup()
    reload()
  }

  /// The shared group when the app is signed with it, else nil: the app's
  /// own Keychain (simulator builds without signing).
  private static func usableGroup() -> String? {
    guard let group = accessGroup else { return nil }
    let probe: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: "it.cipi1965.compass.probe",
      kSecAttrAccessGroup as String: group, kSecMatchLimit as String: kSecMatchLimitOne,
    ]
    return SecItemCopyMatching(probe as CFDictionary, nil) != errSecMissingEntitlement ? group : nil
  }

  func reload() {
    all = KeychainPasswordStore.everyProfile(accessGroup: group).sorted { ($0.host, $0.profile) < ($1.host, $1.profile) }
    updateIdentityStore()
  }

  /// Saved logins for a page of a profile.
  func matches(for url: URL?, profile: String) -> [SavedPassword] {
    guard let url else { return [] }
    return PasswordsBridge.matches(all.filter { $0.profile == profile }, for: url)
  }

  func hasForm(_ tabID: String) -> Bool { formTabs.contains(tabID) }

  /// A message from the page script.
  func handle(_ body: Any, tabID: String, pageURL: URL?, profile: String) {
    guard let message = body as? [String: Any], let type = message["type"] as? String else { return }
    switch type {
    case "form":
      formTabs.insert(tabID)
    case "submit":
      guard let url = pageURL, let scheme = url.scheme, let host = url.host(),
        let password = message["password"] as? String, !password.isEmpty
      else { return }
      let realm = "\(scheme.lowercased())://\(host.lowercased())/"
      let mine = all.filter { $0.profile == profile }
      guard !PasswordsBridge.isBlocked(mine, for: url) else { return }
      let username = message["username"] as? String ?? ""
      let existing = mine.first { $0.signonRealm == realm && $0.username == username }
      if existing?.password == password { return }
      pendingSave = PendingSave(
        origin: url.absoluteString, realm: realm, username: username, password: password,
        usernameElement: message["usernameElement"] as? String ?? "",
        passwordElement: message["passwordElement"] as? String ?? "",
        action: message["action"] as? String ?? "", isUpdate: existing != nil, profile: profile)
    default:
      break
    }
  }

  func pageChanged(_ tabID: String) { formTabs.remove(tabID) }

  func save(_ p: PendingSave) {
    if let bridge = bridges[p.profile] {
      Task {
        await bridge.save(
          origin: p.origin, signonRealm: p.realm, username: p.username, password: p.password,
          usernameElement: p.usernameElement, passwordElement: p.passwordElement, action: p.action)
        reload()
      }
    } else {
      let store = store(p.profile)
      store.put(
        PasswordsBridge.makeSaved(
          existing: store.all(), origin: p.origin, signonRealm: p.realm, username: p.username, password: p.password,
          usernameElement: p.usernameElement, passwordElement: p.passwordElement, action: p.action))
      reload()
    }
    pendingSave = nil
  }

  func neverSave(_ p: PendingSave) {
    if let bridge = bridges[p.profile] {
      Task {
        await bridge.neverSave(origin: p.origin, signonRealm: p.realm)
        reload()
      }
    }
    pendingSave = nil
  }

  func delete(_ password: SavedPassword) {
    if let bridge = bridges[password.profile] {
      Task {
        await bridge.delete(password)
        reload()
      }
    } else {
      store(password.profile).delete(tag: password.tag)
      reload()
    }
  }

  func fill(_ password: SavedPassword, in view: WKWebView) {
    view.evaluateJavaScript(
      PasswordScript.fill(username: password.username, password: password.password), in: nil,
      in: PasswordScript.world, completionHandler: nil)
  }

  /// Keeps QuickType suggestions in Safari and other apps in step.
  private func updateIdentityStore() {
    let identities = all.filter { !$0.isBlocked && !$0.password.isEmpty }.map {
      ASPasswordCredentialIdentity(
        serviceIdentifier: ASCredentialServiceIdentifier(identifier: $0.host, type: .domain), user: $0.username,
        recordIdentifier: $0.recordID)
    }
    ASCredentialIdentityStore.shared.getState { state in
      guard state.isEnabled else { return }
      ASCredentialIdentityStore.shared.replaceCredentialIdentities(identities) { _, _ in }
    }
  }
}
