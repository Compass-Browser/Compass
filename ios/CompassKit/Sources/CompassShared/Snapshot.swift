// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

// What the app's extensions (Share, Widgets, Safari) know of its state: a
// read-only picture of the spaces, written by the app to the App Group after
// each change (docs/design/ios.md, "System extensions"). Extensions never
// open Sidebar.json or the sync store; they read this and ask the app for
// changes through the Inbox.

/// The app's state as the extensions see it.
public struct Snapshot: Codable, Equatable, Sendable {
  public static let currentVersion = 1

  public var version = Snapshot.currentVersion
  public var written: Date
  /// In the sidebar's order.
  public var spaces: [SpaceInfo]
  /// Space IDs, the most recently shown first.
  public var recentSpaceIDs: [String]
  /// Other devices' open tabs (synced sessions), the most recently changed
  /// device first. Empty while sync is off.
  public var devices: [DeviceTabs]
  public var syncOn: Bool

  public init(
    written: Date = Date(), spaces: [SpaceInfo] = [], recentSpaceIDs: [String] = [], devices: [DeviceTabs] = [],
    syncOn: Bool = false
  ) {
    self.written = written
    self.spaces = spaces
    self.recentSpaceIDs = recentSpaceIDs
    self.devices = devices
    self.syncOn = syncOn
  }

  public func space(id: String?) -> SpaceInfo? {
    guard let id else { return nil }
    return spaces.first { $0.id == id }
  }

  /// The space shown last, or the first one.
  public var currentSpace: SpaceInfo? {
    recentSpaceIDs.lazy.compactMap { space(id: $0) }.first ?? spaces.first
  }

  /// Up to `count` spaces, the most recently shown first, then the rest in
  /// sidebar order.
  public func recentSpaces(_ count: Int) -> [SpaceInfo] {
    var out = recentSpaceIDs.compactMap { space(id: $0) }
    for s in spaces where !out.contains(where: { $0.id == s.id }) { out.append(s) }
    return Array(out.prefix(count))
  }
}

/// A space, with what its widgets and the share sheet show.
public struct SpaceInfo: Codable, Equatable, Sendable, Identifiable {
  public var id: String
  public var name: String
  public var icon: SpaceGlyph
  public var style: SpaceStyle
  /// Its profile's favorites (shared by the profile's spaces).
  public var favorites: [Entry]
  /// Its Pinned tabs, depth first (folders flattened, a split as its first
  /// pane); at most `Snapshot.maxEntries`.
  public var pinned: [Entry]
  /// Its Pinned folders as a tree, for "Add to Pinned" in a folder.
  public var folders: [Folder]
  /// Its Today tabs, the newest first; at most `Snapshot.maxEntries`.
  public var today: [Entry]
  /// How many Today entries there are in all.
  public var todayCount: Int

  public init(
    id: String, name: String, icon: SpaceGlyph = .initial("•"), style: SpaceStyle = SpaceStyle(),
    favorites: [Entry] = [], pinned: [Entry] = [], folders: [Folder] = [], today: [Entry] = [], todayCount: Int = 0
  ) {
    self.id = id
    self.name = name
    self.icon = icon
    self.style = style
    self.favorites = favorites
    self.pinned = pinned
    self.folders = folders
    self.today = today
    self.todayCount = todayCount
  }

  /// Every folder, depth first, with its depth (0 = top level).
  public var allFolders: [(folder: Folder, depth: Int)] {
    func walk(_ list: [Folder], _ depth: Int) -> [(Folder, Int)] {
      list.flatMap { [($0, depth)] + walk($0.children, depth + 1) }
    }
    return walk(folders, 0)
  }

  public func folder(id: String?) -> Folder? {
    guard let id else { return nil }
    return allFolders.first { $0.folder.id == id }?.folder
  }
}

extension Snapshot {
  /// What a space keeps of each list: more than the large widget shows.
  public static let maxEntries = 16
}

/// A space's symbol: an SF Symbol name, an emoji, or the name's initial.
public enum SpaceGlyph: Codable, Equatable, Sendable {
  case symbol(String)
  case emoji(String)
  case initial(String)
}

/// A space's colors, as the app paints its sidebar: tint stops at an opacity
/// over the system background, and an accent for icons.
public struct SpaceStyle: Codable, Equatable, Sendable {
  /// 0xRRGGBB, already turned into tint colors; empty = no tint (grey).
  public var stops: [UInt32]
  /// The stops' opacity, 0 to 1.
  public var opacity: Double
  /// 0xRRGGBB.
  public var accent: UInt32
  /// nil follows the system; else "light" or "dark".
  public var scheme: String?

  public init(stops: [UInt32] = [], opacity: Double = 0, accent: UInt32 = 0x8C919E, scheme: String? = nil) {
    self.stops = stops
    self.opacity = opacity
    self.accent = accent
    self.scheme = scheme
  }
}

/// A tab in a list.
public struct Entry: Codable, Equatable, Sendable, Identifiable {
  /// The sidebar node's ID (for a device's tab, its URL).
  public var id: String
  public var title: String
  public var url: String

  public init(id: String, title: String, url: String) {
    self.id = id
    self.title = title
    self.url = url
  }

  /// The URL's host, lowercased, without "www.".
  public var host: String {
    let host = URL(string: url)?.host()?.lowercased() ?? ""
    return host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
  }

  /// The full host, lowercased: the name of its icon (SharedContainer.icon).
  public var iconHost: String { URL(string: url)?.host()?.lowercased() ?? "" }

  /// The title, or the host when it has none.
  public var label: String { title.isEmpty ? (host.isEmpty ? url : host) : title }
}

/// A Pinned folder.
public struct Folder: Codable, Equatable, Sendable, Identifiable {
  public var id: String
  public var name: String
  public var children: [Folder]

  public init(id: String, name: String, children: [Folder] = []) {
    self.id = id
    self.name = name
    self.children = children
  }
}

/// Another device's open tabs.
public struct DeviceTabs: Codable, Equatable, Sendable, Identifiable {
  public var id: String
  public var name: String
  /// "desktop", "phone" or "tablet".
  public var kind: String
  public var modified: Date
  public var tabs: [Entry]

  public init(id: String, name: String, kind: String, modified: Date, tabs: [Entry]) {
    self.id = id
    self.name = name
    self.kind = kind
    self.modified = modified
    self.tabs = tabs
  }
}
