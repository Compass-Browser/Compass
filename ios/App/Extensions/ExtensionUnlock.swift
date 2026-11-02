// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import LocalAuthentication
import Security
import UIKit
import WebKit

/// Unlock with Face ID for extensions that ask for a password (1Password):
/// on desktop Chrome they unlock with the desktop app's biometrics over
/// native messaging, which no app can offer on iPhone. Instead, a script in
/// the extension's own pages (popup, options, its pages in tabs) watches for
/// a password field:
///
/// - once the user types the password and the field goes away (unlocked),
///   Compass offers to keep it, in the Keychain, on this device only and
///   behind Face ID (biometryCurrentSet: a new face or finger drops it);
/// - from then on, when the field shows up, Face ID fills and submits it;
///   if the field is still there 10 seconds later, the saved password is
///   wrong (changed elsewhere): it's forgotten and the user types it once.
///
/// docs/design/ios.md, "Web extensions".
@MainActor
final class ExtensionUnlock: NSObject {
  static let shared = ExtensionUnlock()
  static let handler = "compassUnlock"
  private nonisolated static let service = "it.cipi1965.compass.extension-unlock"
  private static let declinedKey = "CompassExtensionUnlockDeclined"

  /// Web views where Face ID was just tried, so one field isn't asked for
  /// twice (a re-render, a second field).
  private let recentlyFilled = NSHashTable<WKWebView>.weakObjects()
  private var prompting = false

  /// Whether this device can use it (Face ID or Touch ID enrolled).
  static var isAvailable: Bool {
    LAContext().canEvaluatePolicy(.deviceOwnerAuthenticationWithBiometrics, error: nil)
  }

  static var biometryName: String {
    let context = LAContext()
    _ = context.canEvaluatePolicy(.deviceOwnerAuthenticationWithBiometrics, error: nil)
    return context.biometryType == .touchID ? "Touch ID" : "Face ID"
  }

  // MARK: Pages

  /// An extension page already loaded (the action's popup).
  func attach(to view: WKWebView, extensionID: String, name: String) {
    guard Self.isAvailable else { return }
    let controller = view.configuration.userContentController
    controller.removeScriptMessageHandler(forName: Self.handler)
    controller.add(Proxy(self), name: Self.handler)
    view.evaluateJavaScript(Self.script(extensionID: extensionID, name: name), completionHandler: nil)
  }

  /// A configuration for extension pages yet to load (tabs, options).
  func attach(to configuration: WKWebViewConfiguration, extensionID: String, name: String) {
    guard Self.isAvailable else { return }
    let controller = configuration.userContentController
    controller.removeScriptMessageHandler(forName: Self.handler)
    controller.add(Proxy(self), name: Self.handler)
    controller.addUserScript(WKUserScript(
      source: Self.script(extensionID: extensionID, name: name), injectionTime: .atDocumentEnd,
      forMainFrameOnly: true))
  }

  fileprivate func received(_ body: Any, from view: WKWebView?) {
    guard let view, let message = body as? [String: Any], let event = message["event"] as? String,
      let id = message["id"] as? String, let name = message["name"] as? String,
      view.url?.scheme == "webkit-extension"
    else { return }
    switch event {
    case "field":
      fill(view, id: id, name: name)
    case "unlocked":
      guard let password = message["password"] as? String, !password.isEmpty else { return }
      offerToSave(password, id: id, name: name)
    case "fillFailed":
      Self.forget(id)
      let alert = UIAlertController(
        title: "\(Self.biometryName) unlock turned off",
        message: "The saved password for \(name) didn't work. Type it once to turn \(Self.biometryName) back on.",
        preferredStyle: .alert)
      alert.addAction(UIAlertAction(title: "OK", style: .default))
      Presenter.present(alert)
    default:
      break
    }
  }

  private func fill(_ view: WKWebView, id: String, name: String) {
    guard Self.isSaved(id), !recentlyFilled.contains(view), !prompting else { return }
    recentlyFilled.add(view)
    prompting = true
    Task {
      let password = await Self.read(id, reason: "Unlock \(name)")
      prompting = false
      guard let password, let json = String(data: try! JSONEncoder().encode([password]), encoding: .utf8) else {
        // Cancelled: the field stays for typing; Face ID again next time.
        recentlyFilled.remove(view)
        return
      }
      _ = try? await view.evaluateJavaScript("window.__compassUnlockFill && window.__compassUnlockFill(\(json)[0])")
      // A fresh field later (locked again) can be filled again.
      try? await Task.sleep(for: .seconds(10))
      recentlyFilled.remove(view)
    }
  }

  private func offerToSave(_ password: String, id: String, name: String) {
    let declined = Set(UserDefaults.standard.stringArray(forKey: Self.declinedKey) ?? [])
    guard !Self.isSaved(id), !declined.contains(id) else { return }
    #if DEBUG
      //   -CompassUnlockAutoSave YES: "Use Face ID" without the alert.
      if UserDefaults.standard.bool(forKey: "CompassUnlockAutoSave") {
        print("WebExtensions: unlock saved for \(id)")
        return Self.save(password, id)
      }
    #endif
    let biometry = Self.biometryName
    let alert = UIAlertController(
      title: "Unlock \(name) with \(biometry)?",
      message: "Compass keeps the password on this iPhone, behind \(biometry), and fills it in when \(name) asks.",
      preferredStyle: .alert)
    alert.addAction(UIAlertAction(title: "Not Now", style: .cancel) { _ in
      UserDefaults.standard.set(Array(declined.union([id])), forKey: Self.declinedKey)
    })
    alert.addAction(UIAlertAction(title: "Use \(biometry)", style: .default) { _ in
      Self.save(password, id)
    })
    Presenter.present(alert)
  }

  // MARK: Keychain

  private static func base(_ id: String) -> [String: Any] {
    [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: service,
      kSecAttrAccount as String: id,
    ]
  }

  /// Whether a password is kept for the extension (no Face ID asked).
  static func isSaved(_ id: String) -> Bool {
    let context = LAContext()
    context.interactionNotAllowed = true
    var query = base(id)
    query[kSecUseAuthenticationContext as String] = context
    query[kSecReturnAttributes as String] = true
    let status = SecItemCopyMatching(query as CFDictionary, nil)
    return status == errSecSuccess || status == errSecInteractionNotAllowed
  }

  static func save(_ password: String, _ id: String) {
    SecItemDelete(base(id) as CFDictionary)
    guard let access = SecAccessControlCreateWithFlags(
      nil, kSecAttrAccessibleWhenPasscodeSetThisDeviceOnly, .biometryCurrentSet, nil)
    else { return }
    var query = base(id)
    query[kSecValueData as String] = Data(password.utf8)
    query[kSecAttrAccessControl as String] = access
    SecItemAdd(query as CFDictionary, nil)

    UserDefaults.standard.set(
      (UserDefaults.standard.stringArray(forKey: declinedKey) ?? []).filter { $0 != id }, forKey: declinedKey)
  }

  static func forget(_ id: String) {
    SecItemDelete(base(id) as CFDictionary)
  }

  /// The password, after Face ID (off the main thread: the read waits for
  /// it). nil when cancelled or gone.
  static func read(_ id: String, reason: String) async -> String? {
    await Task.detached { readBlocking(id, reason: reason) }.value
  }

  private nonisolated static func readBlocking(_ id: String, reason: String) -> String? {
    let context = LAContext()
    context.localizedReason = reason
    let query: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: service,
      kSecAttrAccount as String: id,
      kSecUseAuthenticationContext as String: context,
      kSecReturnData as String: true,
      kSecMatchLimit as String: kSecMatchLimitOne,
    ]
    var result: CFTypeRef?
    guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess, let data = result as? Data else {
      return nil
    }
    return String(data: data, encoding: .utf8)
  }

  // MARK: Script

  /// Watches for a visible password field: reports it ("field"), keeps what
  /// the user submits, and reports it once the field is gone ("unlocked").
  /// `__compassUnlockFill` types a password the way a user would (React and
  /// friends listen to input events) and submits; if the field is still
  /// there 10 s later (1Password's unlock takes seconds), "fillFailed".
  static func script(extensionID: String, name: String) -> String {
    let ids = String(data: try! JSONEncoder().encode(["id": extensionID, "name": name]), encoding: .utf8)!
    return """
      (() => {
        if (window.__compassUnlock) return;
        window.__compassUnlock = true;
        const ids = \(ids);
        const post = (m) => { try { webkit.messageHandlers.\(handler).postMessage(Object.assign(m, ids)); } catch (e) {} };
        const visible = (el) => el && el.isConnected && !el.disabled && el.getClientRects().length > 0;
        const find = () => Array.from(document.querySelectorAll("input[type=password]")).find(visible);
        let field = null, typed = null, filled = false;
        const remember = () => { const f = find(); if (f && f.value && !filled) typed = f.value; };
        document.addEventListener("keydown", (e) => { if (e.key === "Enter" && e.target && e.target.type === "password") remember(); }, true);
        document.addEventListener("submit", remember, true);
        document.addEventListener("pointerdown", (e) => {
          if (e.target && e.target.closest && e.target.closest("button, [type=submit], [role=button]")) remember();
        }, true);
        const check = () => {
          const f = find();
          if (f && f !== field) { field = f; post({ event: "field" }); }
          if (!f && field) {
            field = null;
            if (typed) post({ event: "unlocked", password: typed });
            typed = null;
            filled = false;
          }
        };
        new MutationObserver(check).observe(document.documentElement, { childList: true, subtree: true, attributes: true });
        check();
        window.__compassUnlockFill = (password) => {
          const f = find();
          if (!f) return false;
          filled = true;
          typed = null;
          f.focus();
          Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, "value").set.call(f, password);
          f.dispatchEvent(new Event("input", { bubbles: true }));
          f.dispatchEvent(new Event("change", { bubbles: true }));
          setTimeout(() => {
            if (f.form && f.form.requestSubmit) { f.form.requestSubmit(); }
            else {
              for (const type of ["keydown", "keypress", "keyup"]) {
                f.dispatchEvent(new KeyboardEvent(type, { key: "Enter", code: "Enter", keyCode: 13, which: 13, bubbles: true }));
              }
            }
          }, 60);
          setTimeout(() => { if (visible(f) && f === find()) post({ event: "fillFailed" }); }, 10000);
          return true;
        };
      })();
      """
  }
}

/// Forwards script messages without the content controller retaining the
/// unlock helper.
private final class Proxy: NSObject, WKScriptMessageHandler {
  weak var target: ExtensionUnlock?
  init(_ target: ExtensionUnlock) { self.target = target }

  func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
    MainActor.assumeIsolated { target?.received(message.body, from: message.webView) }
  }
}
