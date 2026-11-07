// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import CompassShared
import CompassSync
import Foundation
import Observation
import UIKit
import WidgetKit

/// The app's side of its system extensions (docs/design/ios.md, "System
/// extensions"):
///  - writes the Snapshot (and site icons) to the App Group after changes,
///    then reloads the widgets, the Spotlight index and the icon menu;
///  - takes the Inbox's requests (Share, Safari) and the Focus filter's space;
///  - opens `compass:` links.
@MainActor
@Observable
final class SystemBridge {
  /// The App Group's folder; the app's own when the build has no group (an
  /// unsigned simulator build), so the app works the same, unseen.
  @ObservationIgnored let container: SharedContainer
  /// The Focus filter's setting while its Focus is on.
  private(set) var focus: FocusSetting?
  /// The window the user is in (AppServices.activate).
  @ObservationIgnored weak var app: AppModel? {
    didSet { if app != nil, let link = pendingLink { pendingLink = nil; handle(link) } }
  }
  /// A link (widget, quick action) that came before the first window.
  @ObservationIgnored private var pendingLink: DeepLink?
  @ObservationIgnored private var snapshotTask: Task<Void, Never>?
  @ObservationIgnored private var iconTask: Task<Void, Never>?
  @ObservationIgnored private var hosts: Set<String> = []
  @ObservationIgnored private let spotlight = Spotlight()
  /// After a launch the index and the icon menu are set even when the
  /// snapshot didn't change.
  @ObservationIgnored private var wroteOnce = false

  private static let recentKey = "CompassRecentSpaces"

  init() {
    container = .current
    focus = container.readFocus()
    let observer = Unmanaged.passUnretained(self).toOpaque()
    CFNotificationCenterAddObserver(
      CFNotificationCenterGetDarwinNotifyCenter(), observer,
      { _, observer, _, _, _ in
        guard let observer else { return }
        let bridge = Unmanaged<SystemBridge>.fromOpaque(observer).takeUnretainedValue()
        Task { @MainActor in bridge.sharedChanged() }
      }, SharedContainer.changedNotification as CFString, nil, .deliverImmediately)
  }

  // MARK: Recent spaces

  /// Space IDs, the most recently shown first.
  var recentSpaceIDs: [String] {
    get { UserDefaults.standard.stringArray(forKey: Self.recentKey) ?? [] }
    set { UserDefaults.standard.set(Array(newValue.prefix(8)), forKey: Self.recentKey) }
  }

  func spaceShown(_ id: String) {
    guard recentSpaceIDs.first != id else { return }
    recentSpaceIDs = [id] + recentSpaceIDs.filter { $0 != id }
    scheduleSnapshot()
  }

  // MARK: Snapshot

  /// Writes the snapshot a second after the last change.
  func scheduleSnapshot() {
    snapshotTask?.cancel()
    snapshotTask = Task { [weak self] in
      try? await Task.sleep(for: .seconds(1))
      guard !Task.isCancelled else { return }
      self?.writeSnapshot()
    }
  }

  /// `force`: update the index and the icon menu even if nothing changed.
  func writeSnapshot(force: Bool = false) {
    if force { wroteOnce = false }
    guard let app else { return }
    let snapshot = app.store.snapshot(
      style: Self.style, glyph: Self.glyph, recentSpaceIDs: recentSpaceIDs,
      devices: Self.devices(app.sync), syncOn: app.sync.isOn)
    guard (try? container.write(snapshot)) == true || !wroteOnce else { return }
    wroteOnce = true
    hosts = Self.hosts(in: snapshot)
    copyIcons(hosts)
    WidgetCenter.shared.reloadAllTimelines()
    spotlight.update(snapshot, icons: container, enabled: Spotlight.isEnabled)
    HomeQuickActions.update(snapshot)
  }

  static func style(_ space: Space) -> SpaceStyle {
    let theme = space.theme
    let scheme: String? =
      switch theme.colorScheme {
      case .light: "light"
      case .dark: "dark"
      default: nil
      }
    return SpaceStyle(
      stops: theme.stops.map(\.tint.hex), opacity: min(theme.intensity * SpaceTint.maxOpacity, 1),
      accent: theme.color?.hex ?? 0x8C919E, scheme: scheme)
  }

  static func glyph(_ space: Space) -> SpaceGlyph {
    switch SpaceIcon.parse(space.icon) {
    case .symbol(let name): .symbol(name)
    case .emoji(let emoji): .emoji(emoji)
    case nil: .initial(space.name.first.map { String($0).uppercased() } ?? "•")
    }
  }

  /// Every account's other devices, newest first, the same device once.
  static func devices(_ sync: SyncController) -> [DeviceTabs] {
    var seen: Set<String> = []
    let sessions = sync.sessionsByAccount.values.flatMap { $0 }.sorted { $0.modified > $1.modified }
    return sessions.compactMap { session -> DeviceTabs? in
      guard seen.insert(session.tag).inserted else { return nil }
      let tabs = session.tabs.filter { PhoneVisible.url($0.url) }.sorted { $0.lastActive > $1.lastActive }
        .prefix(Snapshot.maxEntries).map { Entry(id: $0.url, title: $0.title, url: $0.url) }
      guard !tabs.isEmpty else { return nil }
      let kind =
        switch session.formFactor {
        case .phone: "phone"
        case .tablet: "tablet"
        default: "desktop"
        }
      return DeviceTabs(id: session.tag, name: session.name, kind: kind, modified: session.modified, tabs: Array(tabs))
    }.prefix(4).map { $0 }
  }

  // MARK: Icons

  private static func hosts(in snapshot: Snapshot) -> Set<String> {
    let entries =
      snapshot.spaces.flatMap { $0.favorites + $0.pinned + $0.today } + snapshot.devices.flatMap(\.tabs)
    return Set(entries.compactMap { Favicons.host(of: $0.url) })
  }

  /// Copies the site icons the snapshot's entries need (and asks Favicons
  /// for those it hasn't got yet; `iconStored` brings them later).
  private func copyIcons(_ hosts: Set<String>) {
    try? FileManager.default.createDirectory(at: container.iconsDirectory, withIntermediateDirectories: true)
    for host in hosts {
      let target = container.icon(host: host)
      guard let source = Favicons.shared.cachedFile(host: host) else {
        _ = Favicons.shared.image(for: "https://\(host)/")
        continue
      }
      let newer =
        (try? source.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate)
        ?? .distantPast
      let old =
        (try? target.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
      guard newer > old else { continue }
      try? FileManager.default.removeItem(at: target)
      try? FileManager.default.copyItem(at: source, to: target)
    }
  }

  /// Favicons saved a host's icon: copy it if a widget shows that host.
  func iconStored(host: String) {
    guard hosts.contains(host) else { return }
    iconTask?.cancel()
    iconTask = Task { [weak self] in
      try? await Task.sleep(for: .seconds(2))
      guard let self, !Task.isCancelled else { return }
      self.copyIcons(self.hosts)
      WidgetCenter.shared.reloadAllTimelines()
    }
  }

  // MARK: Inbox and Focus

  /// An extension changed something: take the Inbox, follow the Focus.
  func sharedChanged() {
    drainInbox()
    readFocus()
  }

  func drainInbox() {
    guard let app else { return }
    for item in container.inbox.drain() {
      guard let url = URL(string: item.url), PhoneVisible.url(item.url) else { continue }
      let space = item.spaceID.flatMap(app.store.space(id:))
      switch item.action {
      case .open:
        app.openLink(url, in: space)
      case .pin:
        guard let space = space ?? app.selectedSpace else { continue }
        let parent = item.folderID.flatMap(app.store.node(id:)).flatMap { $0.isFolder ? $0 : nil } ?? space.pinned
        app.store.addTab(url: item.url, title: item.title, to: parent)
      }
    }
  }

  /// The Focus filter's space: shown when its Focus comes on.
  func readFocus() {
    let new = container.readFocus()
    guard new != focus else { return }
    focus = new
    if let id = new?.spaceID, let app, app.store.space(id: id) != nil { app.selectedSpaceID = id }
    WidgetCenter.shared.reloadAllTimelines()
  }

  // MARK: Links

  func handle(_ link: DeepLink) {
    guard let app else {
      pendingLink = link
      return
    }
    switch link {
    case .open(let text, let space):
      guard let url = URL(string: text), PhoneVisible.url(text) else { return }
      app.openLink(url, in: space.flatMap(app.store.space(id:)))
    case .node(let id):
      guard let node = app.store.node(id: id) else { return }
      app.dismissOverlays()
      app.show(node)
    case .space(let id):
      guard app.store.space(id: id) != nil else { return }
      app.dismissOverlays()
      app.selectedSpaceID = id
      app.showSidebar()
    case .search:
      app.dismissOverlays()
      app.openCommandBar(newTab: true)
    case .newTab(let space):
      app.dismissOverlays()
      if let space, app.store.space(id: space) != nil { app.selectedSpaceID = space }
      app.openCommandBar(newTab: true)
    case .incognito:
      app.dismissOverlays()
      app.openIncognitoTab()
    case .inbox:
      drainInbox()
    case .downloads:
      app.dismissOverlays()
      app.showingDownloads = true
    }
  }
}
