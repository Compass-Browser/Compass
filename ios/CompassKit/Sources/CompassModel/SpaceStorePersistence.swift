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
    var color: Int32
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
          id: s.id, name: s.name, color: s.color.rawValue, icon: s.icon, profileUUID: s.profileUUID,
          archiveAfter: s.archiveAfter, position: Data(s.position.compressed),
          pinned: s.pinned.children.map(node), today: s.today.children.map(node),
          archive: s.archive.children.map(node), unknownFields: s.unknownFields)
      }, favorites: favorites.children.map(node))
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
      for child in favorites.children { rawDetach(child) }
      for record in snapshot.spaces {
        let space = makeSpace(
          id: record.id, name: record.name, color: SpaceColor(rawValue: record.color) ?? .blue, icon: record.icon,
          profileUUID: record.profileUUID, archiveAfter: record.archiveAfter,
          position: UniquePosition(compressed: [UInt8](record.position)))
        space.unknownFields = record.unknownFields
        record.pinned.forEach { build($0, into: space.pinned) }
        record.today.forEach { build($0, into: space.today) }
        record.archive.forEach { build($0, into: space.archive) }
        rawInsertSpace(space)
      }
      snapshot.favorites.forEach { build($0, into: favorites) }
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
