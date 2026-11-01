// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// The sidebar as saved on disk ("Compass Sidebar.json" on the desktop; one
/// file for the whole store here). Live state (which tab is loaded) isn't
/// saved; every entry comes back unloaded.
struct SidebarSnapshot: Codable {
  struct Node: Codable {
    var id: String
    var kind: String  // "folder", "tab", "split"
    var title: String
    var customTitle: String
    var url: String
    var lastActive: Date?
    var archivedAt: Date?
    var collapsed: Bool
    var position: Data
    var children: [Node]
    var unknownFields: Data?
  }

  struct SpaceRecord: Codable {
    var id: String
    var name: String
    /// The palette index, or the closest one to `colorRGB`.
    var color: Int32
    /// The theme beyond the palette (absent in files from before it).
    var colorRGB: UInt32?
    var gradient: [UInt32]?
    var intensity: Double?
    var grain: Double?
    var appearance: SpaceAppearance?
    var icon: String
    var profileUUID: String
    var archiveAfter: TimeInterval?
    var position: Data
    var pinned: [Node]
    var today: [Node]
    var archive: [Node]
    var unknownFields: Data?
  }

  var version = 1
  var spaces: [SpaceRecord]
  var favorites: [Node]
  /// Other profiles' favorites, by profile UUID (M8).
  var profileFavorites: [String: [Node]]?
  var homeProfileUUID: String?
}

extension SidebarSnapshot.SpaceRecord {
  var theme: SpaceTheme {
    let color = colorRGB.map(RGB.init(hex:)) ?? (SpaceColor(rawValue: color) ?? .blue).rgb
    return SpaceTheme(
      color: color, gradient: (gradient ?? []).map(RGB.init(hex:)),
      intensity: intensity ?? SpaceTheme.defaultIntensity, grain: grain ?? 0, appearance: appearance ?? .auto)
  }
}

extension SpaceStore {
  public func encodedSnapshot() throws -> Data {
    func node(_ n: SidebarNode) -> SidebarSnapshot.Node {
      let kind: String =
        switch n.kind {
        case .folder: "folder"
        case .split: "split"
        default: "tab"
        }
      return SidebarSnapshot.Node(
        id: n.id, kind: kind, title: n.title, customTitle: n.customTitle, url: n.url,
        lastActive: n.lastActive, archivedAt: n.archivedAt, collapsed: n.collapsed,
        position: Data(n.position.compressed), children: n.children.map(node), unknownFields: n.unknownFields)
    }
    let snapshot = SidebarSnapshot(
      spaces: spaces.map { s in
        SidebarSnapshot.SpaceRecord(
          id: s.id, name: s.name,
          color: (s.theme.color.map { SpaceColor.nearest(to: $0) } ?? .grey).rawValue,
          colorRGB: s.theme.color?.hex, gradient: s.theme.gradient.isEmpty ? nil : s.theme.gradient.map(\.hex),
          intensity: s.theme.intensity, grain: s.theme.grain, appearance: s.theme.appearance,
          icon: s.icon, profileUUID: s.profileUUID,
          archiveAfter: s.archiveAfter, position: Data(s.position.compressed),
          pinned: s.pinned.children.map(node), today: s.today.children.map(node),
          archive: s.archive.children.map(node), unknownFields: s.unknownFields)
      }, favorites: favorites.children.map(node),
      profileFavorites: profileFavorites.isEmpty ? nil : profileFavorites.mapValues { $0.children.map(node) },
      homeProfileUUID: homeProfileUUID.isEmpty ? nil : homeProfileUUID)
    let encoder = JSONEncoder()
    encoder.dateEncodingStrategy = .millisecondsSince1970
    encoder.outputFormatting = .sortedKeys
    return try encoder.encode(snapshot)
  }

  /// Replaces the store's content with a saved snapshot (no change reported).
  public func load(snapshot data: Data) throws {
    let decoder = JSONDecoder()
    decoder.dateDecodingStrategy = .millisecondsSince1970
    let snapshot = try decoder.decode(SidebarSnapshot.self, from: data)
    func build(_ n: SidebarSnapshot.Node, into parent: SidebarNode) {
      let kind: SidebarNode.Kind =
        switch n.kind {
        case "folder": .folder
        case "split": .split
        default: .tab
        }
      let node = makeNode(
        id: n.id, kind: kind, title: n.title, customTitle: n.customTitle, url: n.url, lastActive: n.lastActive,
        archivedAt: n.archivedAt, collapsed: n.collapsed, position: UniquePosition(compressed: [UInt8](n.position)))
      node.unknownFields = n.unknownFields
      rawInsert(node, into: parent)
      n.children.forEach { build($0, into: node) }
    }
    applyingRemote {
      for space in spaces { rawRemoveSpace(space) }
      for root in favoritesRoots { for child in root.children { rawDetach(child) } }
      homeProfileUUID = snapshot.homeProfileUUID ?? ""
      for record in snapshot.spaces {
        let space = makeSpace(
          id: record.id, name: record.name, theme: record.theme, icon: record.icon,
          profileUUID: record.profileUUID, archiveAfter: record.archiveAfter,
          position: UniquePosition(compressed: [UInt8](record.position)))
        space.unknownFields = record.unknownFields
        record.pinned.forEach { build($0, into: space.pinned) }
        record.today.forEach { build($0, into: space.today) }
        record.archive.forEach { build($0, into: space.archive) }
        rawInsertSpace(space)
      }
      snapshot.favorites.forEach { build($0, into: favorites) }
      for (profile, nodes) in snapshot.profileFavorites ?? [:] {
        let root = favorites(forProfile: profile)
        nodes.forEach { build($0, into: root) }
      }
    }
  }

  public func save(to url: URL) throws {
    try FileManager.default.createDirectory(
      at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try encodedSnapshot().write(to: url, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
  }

  public func load(from url: URL) throws {
    try load(snapshot: Data(contentsOf: url))
  }
}
