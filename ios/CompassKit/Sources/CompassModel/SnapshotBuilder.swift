// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import Foundation

extension SpaceStore {
  /// The extensions' picture of the sidebar (CompassShared.Snapshot). The
  /// app supplies what the model doesn't know: each space's colors and
  /// symbol, the recently shown spaces and the other devices. `limit` caps
  /// each list (Spotlight passes `.max` to get every entry).
  public func snapshot(
    style: (Space) -> SpaceStyle, glyph: (Space) -> SpaceGlyph, recentSpaceIDs: [String] = [],
    devices: [DeviceTabs] = [], syncOn: Bool = false, limit: Int = Snapshot.maxEntries, now: Date = Date()
  ) -> Snapshot {
    let spaces = spaces.map { space -> SpaceInfo in
      let today = Self.webTabs(in: space.today.children).reversed()
      return SpaceInfo(
        id: space.id, name: space.name, icon: glyph(space), style: style(space),
        favorites: Array(Self.webTabs(in: favorites(for: space).children).prefix(limit)),
        pinned: Array(Self.webTabs(in: space.pinned.children).prefix(limit)),
        folders: Self.folders(in: space.pinned),
        today: Array(today.prefix(limit)),
        todayCount: today.count)
    }
    let known = Set(spaces.map(\.id))
    return Snapshot(
      written: now, spaces: spaces, recentSpaceIDs: recentSpaceIDs.filter(known.contains), devices: devices,
      syncOn: syncOn)
  }

  /// Web tabs depth first: a folder's in place, a split as its first web
  /// pane (showing a pane shows its split).
  static func webTabs(in nodes: [SidebarNode]) -> [Entry] {
    nodes.flatMap { node -> [Entry] in
      switch node.kind {
      case .tab: isWeb(node.url) ? [entry(node)] : []
      case .split: node.children.first { isWeb($0.url) }.map { [entry($0)] } ?? []
      case .folder: webTabs(in: node.children)
      case .root: []
      }
    }
  }

  static func folders(in parent: SidebarNode) -> [Folder] {
    parent.children.filter(\.isFolder).map { Folder(id: $0.id, name: $0.title, children: folders(in: $0)) }
  }

  private static func entry(_ node: SidebarNode) -> Entry {
    Entry(id: node.id, title: node.displayTitle, url: node.url)
  }

  /// What the phone shows (the sidebar's PhoneVisible).
  static func isWeb(_ url: String) -> Bool {
    guard let scheme = URL(string: url)?.scheme?.lowercased() else { return false }
    return scheme == "http" || scheme == "https"
  }
}
