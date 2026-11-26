// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import CompassSync
import UIKit
import UserNotifications

/// Tabs other devices send to this phone (docs/design/send-tab.md), as
/// notifications: "Tab from MacBook", the page's title and address. A tap
/// opens the tab in a space of the account it came in and marks it opened,
/// so the sender stops offering it. Shown in the app too (as a banner).
@MainActor
final class SentTabNotifications: NSObject, UNUserNotificationCenterDelegate {
  static let shared = SentTabNotifications()
  /// The window the user is in (AppServices.activate).
  weak var app: AppModel? { didSet { if app != nil, let open = pending { pending = nil; open() } } }
  /// A tap that launched the app, before its first window is up.
  private var pending: (() -> Void)?
  private let center = UNUserNotificationCenter.current()
  nonisolated static let category = "sent-tab"

  /// At launch, before the app finishes: a tap that launched it arrives
  /// through the delegate.
  func setUp(services: AppServices) {
    center.delegate = self
    services.sync.onSentTabs = { [weak self] tabs, account in self?.post(tabs, account: account) }
    // The device token comes each launch. Without notifications allowed
    // it still brings the server's silent pushes (background sync).
    if services.sync.isOn { UIApplication.shared.registerForRemoteNotifications() }
  }

  /// Asked once sync is on (iOS asks the user only the first time).
  func requestAuthorization() {
    // The device token for the server's pushes (AppDelegate gets it): the
    // sent tabs' alerts if allowed, the silent background syncs anyway.
    UIApplication.shared.registerForRemoteNotifications()
    center.requestAuthorization(options: [.alert, .sound, .badge]) { _, _ in }
  }

  func post(_ tabs: [SentTab], account: String) {
    // A push already showed it (the notification service extension).
    center.getDeliveredNotifications { delivered in
      let shown = Set(delivered.compactMap { $0.request.content.userInfo["guid"] as? String })
      let fresh = tabs.filter { !shown.contains($0.guid) }
      guard !fresh.isEmpty else { return }
      Task { @MainActor in self.add(fresh, account: account) }
    }
  }

  private func add(_ tabs: [SentTab], account: String) {
    for tab in tabs {
      let content = UNMutableNotificationContent()
      content.title =
        tab.deviceName.isEmpty
        ? String(localized: "Tab from another device", comment: "Notification title for a tab sent from another device.")
        : String(localized: "Tab from \(tab.deviceName)", comment: "Notification title for a sent tab; the argument is the sending device's name.")
      let host = URL(string: tab.url)?.host() ?? tab.url
      content.body = tab.title.isEmpty || tab.title == tab.url ? tab.url : "\(tab.title)\n\(host)"
      content.sound = .default
      content.threadIdentifier = Self.category
      content.categoryIdentifier = Self.category
      content.userInfo = ["url": tab.url, "guid": tab.guid, "account": account]
      center.add(UNNotificationRequest(identifier: "sent-tab-\(tab.guid)", content: content, trigger: nil))
    }
  }

  private func open(url string: String, guid: String?, account: String) {
    guard let app else {
      pending = { [weak self] in self?.open(url: string, guid: guid, account: account) }
      return
    }
    guard let url = URL(string: string) else { return }
    // A space of the account it came in (the current one when it fits).
    let space = app.selectedSpace.flatMap { app.sync.accountProfile(of: $0) == account ? $0 : nil }
      ?? app.store.spaces.first { app.sync.accountProfile(of: $0) == account }
    app.openLink(url, in: space)
    if let guid { app.sync.markSentTabOpened(guid, account: account) }
  }

  // The completion-handler form, on the main actor (iOS calls the delegate
  // on the main thread): the async form completes off it, and UIKit asserts
  // (a crash on every tap).
  nonisolated func userNotificationCenter(
    _ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse,
    withCompletionHandler completionHandler: @escaping () -> Void
  ) {
    let content = response.notification.request.content
    if content.categoryIdentifier == Self.category,
      response.actionIdentifier == UNNotificationDefaultActionIdentifier,
      let url = content.userInfo["url"] as? String
    {
      let guid = content.userInfo["guid"] as? String
      let account = content.userInfo["account"] as? String ?? ""
      MainActor.assumeIsolated { open(url: url, guid: guid, account: account) }
    }
    completionHandler()
  }

  nonisolated func userNotificationCenter(
    _ center: UNUserNotificationCenter, willPresent notification: UNNotification,
    withCompletionHandler completionHandler: @escaping (UNNotificationPresentationOptions) -> Void
  ) {
    completionHandler([.banner, .list, .sound])
  }
}
