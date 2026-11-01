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
/// AutoFill identity list. Saves go through sync when it's on.
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

  let store: any PasswordStore
  private(set) var all: [SavedPassword] = []
  /// Tabs whose page has a password field.
  private(set) var formTabs: Set<String> = []
  /// A login just typed into a page, waiting for "Save?".
  var pendingSave: PendingSave?
  var bridge: PasswordsBridge? {
    didSet {
      bridge?.onChange = { [weak self] _ in Task { @MainActor in self?.reload() } }
    }
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
  }

  init() {
    store = Self.makeStore()
    reload()
  }

  /// The shared group when the app is signed with it, else the app's own
  /// Keychain (simulator builds without signing).
  private static func makeStore() -> any PasswordStore {
    if let group = accessGroup {
      let probe: [String: Any] = [
        kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: "it.cipi1965.compass.probe",
        kSecAttrAccessGroup as String: group, kSecMatchLimit as String: kSecMatchLimitOne,
      ]
      if SecItemCopyMatching(probe as CFDictionary, nil) != errSecMissingEntitlement {
        return KeychainPasswordStore(accessGroup: group)
      }
    }
    return KeychainPasswordStore(accessGroup: nil)
  }

  func reload() {
    all = store.all().sorted { $0.host < $1.host }
    updateIdentityStore()
  }

  func matches(for url: URL?) -> [SavedPassword] {
    guard let url else { return [] }
    return PasswordsBridge.matches(all, for: url)
  }

  func hasForm(_ tabID: String) -> Bool { formTabs.contains(tabID) }

  /// A message from the page script.
  func handle(_ body: Any, tabID: String, pageURL: URL?) {
    guard let message = body as? [String: Any], let type = message["type"] as? String else { return }
    switch type {
    case "form":
      formTabs.insert(tabID)
    case "submit":
      guard let url = pageURL, let scheme = url.scheme, let host = url.host(),
        let password = message["password"] as? String, !password.isEmpty
      else { return }
      let realm = "\(scheme.lowercased())://\(host.lowercased())/"
      guard !PasswordsBridge.isBlocked(all, for: url) else { return }
      let username = message["username"] as? String ?? ""
      let existing = all.first { $0.signonRealm == realm && $0.username == username }
      if existing?.password == password { return }
      pendingSave = PendingSave(
        origin: url.absoluteString, realm: realm, username: username, password: password,
        usernameElement: message["usernameElement"] as? String ?? "",
        passwordElement: message["passwordElement"] as? String ?? "",
        action: message["action"] as? String ?? "", isUpdate: existing != nil)
    default:
      break
    }
  }

  func pageChanged(_ tabID: String) { formTabs.remove(tabID) }

  func save(_ p: PendingSave) {
    if let bridge {
      Task {
        await bridge.save(
          origin: p.origin, signonRealm: p.realm, username: p.username, password: p.password,
          usernameElement: p.usernameElement, passwordElement: p.passwordElement, action: p.action)
        reload()
      }
    } else {
      store.put(
        PasswordsBridge.makeSaved(
          existing: all, origin: p.origin, signonRealm: p.realm, username: p.username, password: p.password,
          usernameElement: p.usernameElement, passwordElement: p.passwordElement, action: p.action))
      reload()
    }
    pendingSave = nil
  }

  func neverSave(_ p: PendingSave) {
    if let bridge {
      Task {
        await bridge.neverSave(origin: p.origin, signonRealm: p.realm)
        reload()
      }
    }
    pendingSave = nil
  }

  func delete(_ password: SavedPassword) {
    if let bridge {
      Task {
        await bridge.delete(password)
        reload()
      }
    } else {
      store.delete(tag: password.tag)
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
        recordIdentifier: $0.tag)
    }
    ASCredentialIdentityStore.shared.getState { state in
      guard state.isEnabled else { return }
      ASCredentialIdentityStore.shared.replaceCredentialIdentities(identities) { _, _ in }
    }
  }
}
