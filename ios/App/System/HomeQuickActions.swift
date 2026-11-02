// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import UIKit

/// The menu on the app's Home Screen icon (a long press): New Tab, Search,
/// and the three most recently shown spaces. Each item carries a `compass:`
/// link, handled like a widget's.
enum HomeQuickActions {
  private static let linkKey = "link"

  @MainActor static func update(_ snapshot: Snapshot) {
    var items = [
      item("New Tab", symbol: "plus", link: .newTab(space: nil)),
      item("Search", symbol: "magnifyingglass", link: .search),
    ]
    for space in snapshot.recentSpaces(3) {
      let symbol =
        if case .symbol(let name) = space.icon { name } else { "square.stack" }
      items.append(item(space.name, symbol: symbol, link: .space(space.id), subtitle: "Space"))
    }
    UIApplication.shared.shortcutItems = items
  }

  private static func item(_ title: String, symbol: String, link: DeepLink, subtitle: String? = nil)
    -> UIApplicationShortcutItem
  {
    UIApplicationShortcutItem(
      type: "it.cipi1965.compass.link", localizedTitle: title, localizedSubtitle: subtitle,
      icon: UIApplicationShortcutIcon(systemImageName: symbol),
      userInfo: [linkKey: link.url.absoluteString as NSString])
  }

  static func link(_ item: UIApplicationShortcutItem) -> DeepLink? {
    (item.userInfo?[linkKey] as? String).flatMap(URL.init(string:)).flatMap(DeepLink.init(url:))
  }
}

/// Only for the icon menu: SwiftUI has no hook for its items, so the scene
/// gets a delegate that hands them to the app.
final class AppDelegate: NSObject, UIApplicationDelegate {
  @MainActor static weak var app: AppModel?

  /// The APNs device token (SentTabNotifications asked for it).
  func application(_ application: UIApplication, didRegisterForRemoteNotificationsWithDeviceToken deviceToken: Data) {
    #if DEBUG
      print("Push: device token", deviceToken.map { String(format: "%02x", $0) }.joined())
    #endif
    MainActor.assumeIsolated { AppServices.shared?.sync.setPushToken(deviceToken) }
  }

  func application(_ application: UIApplication, didFailToRegisterForRemoteNotificationsWithError error: Error) {
    #if DEBUG
      print("Push: no device token: \(error)")
    #endif
  }

  /// iOS woke the app for the background download session's events.
  func application(
    _ application: UIApplication, handleEventsForBackgroundURLSession identifier: String,
    completionHandler: @escaping () -> Void
  ) {
    MainActor.assumeIsolated {
      AppServices.shared?.downloads.backgroundCompletion = completionHandler
    }
  }

  func application(
    _ application: UIApplication, configurationForConnecting session: UISceneSession,
    options: UIScene.ConnectionOptions
  ) -> UISceneConfiguration {
    // Launched from the menu: handle it once the app is up.
    if let item = options.shortcutItem, let link = HomeQuickActions.link(item) {
      Task { @MainActor in
        try? await Task.sleep(for: .milliseconds(300))
        AppServices.shared?.system.handle(link)
      }
    }
    let configuration = UISceneConfiguration(name: nil, sessionRole: session.role)
    configuration.delegateClass = SceneDelegate.self
    return configuration
  }
}

final class SceneDelegate: NSObject, UIWindowSceneDelegate {
  func windowScene(
    _ windowScene: UIWindowScene, performActionFor shortcutItem: UIApplicationShortcutItem
  ) async -> Bool {
    guard let link = HomeQuickActions.link(shortcutItem) else { return false }
    await MainActor.run { AppServices.shared?.system.handle(link) }
    return true
  }
}
