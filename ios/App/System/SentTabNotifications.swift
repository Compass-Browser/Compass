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
  weak var app: AppModel?
  private let center = UNUserNotificationCenter.current()
  nonisolated static let category = "sent-tab"

  /// At launch, before the app finishes: a tap that launched it arrives
  /// through the delegate.
  func setUp(app: AppModel) {
    self.app = app
    center.delegate = self
    app.sync.onSentTabs = { [weak self] tabs, account in self?.post(tabs, account: account) }
    // Already allowed (an earlier launch): the device token comes each launch.
    center.getNotificationSettings { settings in
      guard settings.authorizationStatus == .authorized || settings.authorizationStatus == .provisional else { return }
      Task { @MainActor in UIApplication.shared.registerForRemoteNotifications() }
    }
  }

  /// Asked once sync is on (iOS asks the user only the first time).
  func requestAuthorization() {
    center.requestAuthorization(options: [.alert, .sound, .badge]) { granted, _ in
      // The device token for the server's push (AppDelegate gets it).
      guard granted else { return }
      Task { @MainActor in UIApplication.shared.registerForRemoteNotifications() }
    }
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
      content.title = tab.deviceName.isEmpty ? "Tab from another device" : "Tab from \(tab.deviceName)"
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
    guard let app, let url = URL(string: string) else { return }
    // A space of the account it came in (the current one when it fits).
    let space = app.selectedSpace.flatMap { app.sync.accountProfile(of: $0) == account ? $0 : nil }
      ?? app.store.spaces.first { app.sync.accountProfile(of: $0) == account }
    app.openLink(url, in: space)
    if let guid { app.sync.markSentTabOpened(guid, account: account) }
  }

  nonisolated func userNotificationCenter(
    _ center: UNUserNotificationCenter, didReceive response: UNNotificationResponse
  ) async {
    let content = response.notification.request.content
    guard content.categoryIdentifier == Self.category,
      response.actionIdentifier == UNNotificationDefaultActionIdentifier,
      let url = content.userInfo["url"] as? String
    else { return }
    let guid = content.userInfo["guid"] as? String
    let account = content.userInfo["account"] as? String ?? ""
    await MainActor.run { open(url: url, guid: guid, account: account) }
  }

  nonisolated func userNotificationCenter(
    _ center: UNUserNotificationCenter, willPresent notification: UNNotification
  ) async -> UNNotificationPresentationOptions {
    [.banner, .list, .sound]
  }
}
