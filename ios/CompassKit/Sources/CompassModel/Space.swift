// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Observation

/// A space: a name, a theme, and its own pinned, Today and archive trees.
@Observable
public final class Space: Identifiable {
  /// Lowercase UUID, the same on every device.
  public let id: String
  public internal(set) var name: String
  /// Its color, gradient, intensity, grain and mode (SpaceTheme.swift).
  public internal(set) var theme: SpaceTheme
  /// Kept as received (the desktop doesn't use it yet).
  public internal(set) var icon: String
  /// The profile it belongs to; empty = the default profile.
  public internal(set) var profileUUID: String
  /// How long a Today entry may go unseen before it's archived; nil = never.
  public internal(set) var archiveAfter: TimeInterval?
  /// In the browser-wide list of spaces.
  public internal(set) var position: UniquePosition
  public let pinned: SidebarNode
  public let today: SidebarNode
  public let archive: SidebarNode
  @ObservationIgnored public var unknownFields: Data?

  init(
    id: String = UUID().uuidString.lowercased(), name: String, theme: SpaceTheme = .blue, icon: String = "",
    profileUUID: String = "", archiveAfter: TimeInterval? = 12 * 60 * 60,
    position: UniquePosition = UniquePosition(compressed: [])
  ) {
    self.id = id
    self.name = name
    self.theme = theme
    self.icon = icon
    self.profileUUID = profileUUID
    self.archiveAfter = archiveAfter
    self.position = position
    pinned = SidebarNode(id: "pinned:" + id, kind: .root(.pinned))
    today = SidebarNode(id: "today:" + id, kind: .root(.today))
    archive = SidebarNode(id: "archive:" + id, kind: .root(.archive))
  }

  public func root(_ section: Section) -> SidebarNode? {
    switch section {
    case .pinned: pinned
    case .today: today
    case .archive: archive
    case .favorites: nil
    }
  }

  /// Every node of the space, roots excluded.
  public var allNodes: [SidebarNode] {
    [pinned, today, archive].flatMap { $0.children.flatMap(\.subtree) }
  }
}
