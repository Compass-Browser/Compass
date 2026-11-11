// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// The App Group folder the app shares with its extensions:
///
///     Snapshot.json   the spaces as extensions see them (Snapshot)
///     Focus.json      the Focus filter's space (FocusSetting)
///     Inbox/          requests for the app (Inbox)
///     Icons/<host>.png site icons for the snapshot's entries
public struct SharedContainer: Sendable {
  public static let groupID = "group.it.cipi1965.compass"
  /// Posted (Darwin notification) after an extension adds to the Inbox or
  /// changes the Focus, so a running app looks at once.
  public static let changedNotification = "it.cipi1965.compass.shared-changed"

  public let root: URL

  public init(root: URL) {
    self.root = root
  }

  /// The App Group's, or nil when the build isn't entitled to it (a test
  /// run on the Mac; an unsigned build).
  public static var group: SharedContainer? {
    FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: groupID).map(SharedContainer.init(root:))
  }

  /// The group's settings (the share sheet's last space).
  public static var defaults: UserDefaults { UserDefaults(suiteName: groupID) ?? .standard }

  /// Settings → "Share Sheet Opens a Little Window" (in `defaults`): the
  /// share sheet skips its form and opens the link in a little window.
  public static let shareOpensLittleWindowKey = "ShareOpensLittleWindow"
  public static var shareOpensLittleWindow: Bool { defaults.bool(forKey: shareOpensLittleWindowKey) }

  public var snapshotURL: URL { root.appendingPathComponent("Snapshot.json") }
  public var focusURL: URL { root.appendingPathComponent("Focus.json") }
  public var inbox: Inbox { Inbox(directory: root.appendingPathComponent("Inbox", isDirectory: true)) }
  public var iconsDirectory: URL { root.appendingPathComponent("Icons", isDirectory: true) }

  public func icon(host: String) -> URL { iconsDirectory.appendingPathComponent(host + ".png") }

  // MARK: Snapshot

  public func readSnapshot() -> Snapshot? {
    guard let data = try? Data(contentsOf: snapshotURL),
      let snapshot = try? JSONDecoder.shared.decode(Snapshot.self, from: data),
      snapshot.version == Snapshot.currentVersion
    else { return nil }
    return snapshot
  }

  /// Writes it if it changed (`written` aside); whether it did.
  @discardableResult
  public func write(_ snapshot: Snapshot) throws -> Bool {
    if var old = readSnapshot() {
      old.written = snapshot.written
      if old == snapshot { return false }
    }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    try JSONEncoder.shared.encode(snapshot).write(to: snapshotURL, options: .atomic)
    return true
  }

  // MARK: Focus

  public func readFocus() -> FocusSetting? {
    guard let data = try? Data(contentsOf: focusURL) else { return nil }
    return try? JSONDecoder.shared.decode(FocusSetting.self, from: data)
  }

  /// nil clears it (no Focus with a Compass filter is on).
  public func write(_ focus: FocusSetting?) throws {
    guard let focus else {
      try? FileManager.default.removeItem(at: focusURL)
      return
    }
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    try JSONEncoder.shared.encode(focus).write(to: focusURL, options: .atomic)
  }

  // MARK: Notifications

  /// Tells a running app to look at the Inbox and the Focus.
  public static func postChanged() {
    CFNotificationCenterPostNotification(
      CFNotificationCenterGetDarwinNotifyCenter(), CFNotificationName(changedNotification as CFString), nil, nil, true)
  }
}

/// The space a Focus filter chose, while that Focus is on.
public struct FocusSetting: Codable, Equatable, Sendable {
  public var spaceID: String
  /// The sidebar shows that space only.
  public var onlyThisSpace: Bool

  public init(spaceID: String, onlyThisSpace: Bool) {
    self.spaceID = spaceID
    self.onlyThisSpace = onlyThisSpace
  }
}
