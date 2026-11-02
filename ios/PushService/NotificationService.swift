// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassShared
import CompassSync
import Foundation
import Security
import UserNotifications

/// The server's "New tab" push (docs/design/send-tab.md) becomes "Tab from
/// MacBook", the page's title and host: one download of SEND_TAB_TO_SELF
/// per account of this phone, with the chain's keys from the shared
/// Keychain group and the accounts the app lists in the App Group. The
/// newest tab not shown yet wins; if there's none for this phone (the tab
/// was for another device), the notification says so.
final class NotificationService: UNNotificationServiceExtension {
  private var box: Box?
  private var work: Task<Void, Never>?

  override func didReceive(
    _ request: UNNotificationRequest, withContentHandler contentHandler: @escaping (UNNotificationContent) -> Void
  ) {
    let content = (request.content.mutableCopy() as? UNMutableNotificationContent) ?? UNMutableNotificationContent()
    // Shared with serviceExtensionTimeWillExpire; delivers once.
    let box = Box(content: content, deliver: contentHandler)
    self.box = box
    work = Task {
      let content = box.content
      let found = await Self.newestTab()
      if let (tab, account) = found {
        content.title = tab.deviceName.isEmpty ? "Tab from another device" : "Tab from \(tab.deviceName)"
        let host = URL(string: tab.url)?.host() ?? tab.url
        content.body = tab.title.isEmpty || tab.title == tab.url ? tab.url : "\(tab.title)\n\(host)"
        // As the app's own notifications (SentTabNotifications): a tap opens it.
        content.categoryIdentifier = "sent-tab"
        content.threadIdentifier = "sent-tab"
        content.userInfo = ["url": tab.url, "guid": tab.guid, "account": account]
      } else {
        content.title = "Tab sent"
        content.body = "A tab was sent to another of your devices."
      }
      box.finish()
    }
  }

  /// Out of time: the server's text ("New tab").
  override func serviceExtensionTimeWillExpire() {
    work?.cancel()
    box?.finish()
  }

  private static func newestTab() async -> (SentTab, String)? {
    let group = Bundle.main.object(forInfoDictionaryKey: "CompassKeychainGroup") as? String
    guard let seed = keychain("seed", group: group), let chain = try? Chain(seed: seed),
      let server = keychain("server", group: group).flatMap({ URL(dataRepresentation: $0, relativeTo: nil) }),
      let url = SharedContainer.group?.root.appendingPathComponent("PushAccounts.json"),
      let data = try? Data(contentsOf: url),
      let accounts = try? JSONDecoder().decode([PushAccount].self, from: data)
    else { return nil }
    let shown = Set(
      await UNUserNotificationCenter.current().deliveredNotifications()
        .compactMap { $0.request.content.userInfo["guid"] as? String })
    var best: (SentTab, String)?
    for account in accounts {
      guard
        let tabs = try? await Push.sentTabs(
          server: server, chain: chain, profile: account.profile, cacheGUID: account.cacheGUID)
      else { continue }
      if let tab = tabs.first(where: { !shown.contains($0.guid) }), tab.sharedTime > (best?.0.sharedTime ?? .distantPast) {
        best = (tab, account.profile)
      }
    }
    return best
  }

  private static func keychain(_ account: String, group: String?) -> Data? {
    var query: [String: Any] = [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: "it.cipi1965.compass.sync",
      kSecAttrAccount as String: account,
      kSecReturnData as String: true,
      kSecMatchLimit as String: kSecMatchLimitOne,
    ]
    if let group { query[kSecAttrAccessGroup as String] = group }
    var result: CFTypeRef?
    guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess else { return nil }
    return result as? Data
  }
}

private final class Box: @unchecked Sendable {
  let content: UNMutableNotificationContent
  private let deliver: (UNNotificationContent) -> Void
  private let lock = NSLock()
  private var done = false
  init(content: UNMutableNotificationContent, deliver: @escaping (UNNotificationContent) -> Void) {
    self.content = content
    self.deliver = deliver
  }
  func finish() {
    let first = lock.withLock { () -> Bool in
      defer { done = true }
      return !done
    }
    if first { deliver(content) }
  }
}
