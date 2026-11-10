// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Observation

/// Where a live folder's entries come from: a plugin and the folder's
/// settings for it (the desktop's compass::LiveSource). The part of a live
/// folder that syncs; its entries don't (docs/design/live-folders.md).
public struct LiveSource: Sendable, Equatable, Codable {
  /// The plugin's manifest ID, e.g. "compass.rss".
  public var pluginID: String
  /// The folder's settings, a JSON object.
  public var configJSON: String
  /// An installed plugin's source and the SHA-256 (lowercase hex) of what
  /// was installed; empty for a built-in.
  public var pluginURL: String
  public var pluginSHA256: String

  public init(pluginID: String, configJSON: String = "{}", pluginURL: String = "", pluginSHA256: String = "") {
    self.pluginID = pluginID
    self.configJSON = configJSON
    self.pluginURL = pluginURL
    self.pluginSHA256 = pluginSHA256
  }

  /// The settings as a dictionary (empty if they aren't a JSON object).
  public var config: [String: Any] {
    (try? JSONSerialization.jsonObject(with: Data(configJSON.utf8))) as? [String: Any] ?? [:]
  }

  /// `config` as JSON, keys sorted, so equal settings compare equal.
  public static func json(_ config: [String: Any]) -> String {
    guard JSONSerialization.isValidJSONObject(config),
      let data = try? JSONSerialization.data(withJSONObject: config, options: [.sortedKeys, .withoutEscapingSlashes])
    else { return "{}" }
    return String(decoding: data, as: UTF8.self)
  }
}

/// One entry of a live folder as a refresh delivers it (the plugin's
/// result, normalized by the runtime).
public struct LiveItem: Sendable, Equatable {
  /// The plugin's stable ID for the item; empty or repeated keys are skipped.
  public var key: String
  public var url: String
  public var title: String

  public init(key: String, url: String, title: String) {
    self.key = key
    self.url = url
    self.title = title
  }
}

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
  /// The page a tab shows: follows its live tab's navigations.
  public internal(set) var url: String
  /// A pinned tab's or favorite's own page, which it goes back to when the
  /// user closes it (Arc's "pinned URL"), and that page's title. Set when the
  /// tab lands in Pinned or Favorites, empty elsewhere (the desktop's
  /// SidebarNode::saved_url(); docs/design/sidebar.md, "Pinned URL").
  public internal(set) var savedURL: String = ""
  public internal(set) var savedTitle: String = ""
  /// When a tab was last shown on any device (drives auto-archive).
  public internal(set) var lastActive: Date?
  /// ARCHIVE entries: when they were archived.
  public internal(set) var archivedAt: Date?
  /// Folders: whether only the folder's own row shows.
  public internal(set) var collapsed: Bool
  /// Among its siblings; children are kept sorted by it.
  public internal(set) var position: UniquePosition
  public internal(set) var children: [SidebarNode] = []
  /// A live folder's source; nil for other nodes.
  public internal(set) var liveSource: LiveSource?
  /// When a live folder's entries were last replaced by a refresh.
  public internal(set) var liveRefreshed: Date?
  /// A live folder's entry: the plugin's key for the item it shows. Empty
  /// for every other node, including a tab moved out of its live folder.
  public internal(set) var liveItemKey: String = ""
  /// A live folder's entry that came with a refresh and hasn't been opened.
  public internal(set) var unseen = false
  /// Made a plain folder here, so sync clears its source on the server.
  @ObservationIgnored var liveSourceCleared = false
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

  /// Whether the tab shows another page than its pinned URL (the menu then
  /// offers "Use This Page as Pinned URL" and "Back to Pinned URL").
  public var isAwayFromSavedURL: Bool { isTab && !savedURL.isEmpty && !url.isEmpty && url != savedURL }

  /// Whether this node is in Pinned or Favorites, where tabs keep a pinned
  /// URL; nil while it's detached.
  var keepsSavedURL: Bool? {
    guard let section else { return nil }
    return section == .pinned || section == .favorites
  }

  public var isRoot: Bool { if case .root = kind { true } else { false } }
  public var isTab: Bool { kind == .tab }
  public var isFolder: Bool { kind == .folder }
  public var isSplit: Bool { kind == .split }
  public var isLiveFolder: Bool { liveSource != nil }
  public var isLiveItem: Bool { !liveItemKey.isEmpty }
  /// Whether this node is a live folder or has one below it.
  public var containsLiveFolder: Bool { subtree.contains(where: \.isLiveFolder) }
  /// A live folder's entries that haven't been opened.
  public var unseenCount: Int { isLiveFolder ? children.filter(\.unseen).count : 0 }

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
