// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Observation

/// One node of the sidebar: a section root, a folder, a tab or a split. A
/// Swift port of the desktop's browser/sidebar/sidebar_node.h; nodes change
/// only through SpaceStore, which keeps the tree's invariants.
@Observable
public final class SidebarNode: Identifiable {
  public enum Kind: Sendable, Equatable {
    case root(Section)
    case folder
    case tab
    case split

    public var nodeType: NodeType? {
      switch self {
      case .root: nil
      case .folder: .folder
      case .tab: .tab
      case .split: .split
      }
    }
  }

  /// Lowercase UUID; the same on every synced device (the sync client tag).
  public let id: String
  public let kind: Kind
  /// A tab's page title; a folder's name.
  public internal(set) var title: String
  /// The name the user gave a tab, or empty.
  public internal(set) var customTitle: String
  public internal(set) var url: String
  /// When a tab was last shown on any device (drives auto-archive).
  public internal(set) var lastActive: Date?
  /// ARCHIVE entries: when they were archived.
  public internal(set) var archivedAt: Date?
  /// Folders: whether only the folder's own row shows.
  public internal(set) var collapsed: Bool
  /// Among its siblings; children are kept sorted by it.
  public internal(set) var position: UniquePosition
  public internal(set) var children: [SidebarNode] = []
  @ObservationIgnored public internal(set) weak var parent: SidebarNode?
  /// Synced fields this client doesn't understand, kept as received.
  @ObservationIgnored public var unknownFields: Data?

  init(
    id: String = UUID().uuidString.lowercased(), kind: Kind, title: String = "", customTitle: String = "",
    url: String = "", lastActive: Date? = nil, archivedAt: Date? = nil, collapsed: Bool = false,
    position: UniquePosition = UniquePosition(compressed: [])
  ) {
    self.id = id
    self.kind = kind
    self.title = title
    self.customTitle = customTitle
    self.url = url
    self.lastActive = lastActive
    self.archivedAt = archivedAt
    self.collapsed = collapsed
    self.position = position
  }

  public var displayTitle: String {
    if !customTitle.isEmpty { return customTitle }
    if !title.isEmpty { return title }
    return URL(string: url)?.host() ?? url
  }

  public var isRoot: Bool { if case .root = kind { true } else { false } }
  public var isTab: Bool { kind == .tab }
  public var isFolder: Bool { kind == .folder }
  public var isSplit: Bool { kind == .split }

  /// The section root this node lives under.
  public var root: SidebarNode {
    var node = self
    while let parent = node.parent { node = parent }
    return node
  }

  public var section: Section? {
    if case .root(let section) = root.kind { section } else { nil }
  }

  /// Whether `node` is this node or one of its descendants.
  public func contains(_ node: SidebarNode) -> Bool {
    var current: SidebarNode? = node
    while let c = current {
      if c === self { return true }
      current = c.parent
    }
    return false
  }

  /// This node and every descendant, depth first.
  public var subtree: [SidebarNode] {
    [self] + children.flatMap(\.subtree)
  }

  /// Tabs to open for this entry: itself, a split's panes, a folder's tabs.
  public var tabs: [SidebarNode] {
    switch kind {
    case .tab: [self]
    default: children.flatMap(\.tabs)
    }
  }

  func insert(_ child: SidebarNode) {
    child.parent = self
    let index = children.firstIndex { child.position < $0.position } ?? children.count
    children.insert(child, at: index)
  }

  func remove(_ child: SidebarNode) {
    children.removeAll { $0 === child }
    child.parent = nil
  }

  func resort() {
    children.sort { $0.position < $1.position }
  }
}
