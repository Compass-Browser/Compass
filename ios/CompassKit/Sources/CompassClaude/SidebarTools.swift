// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation

/// The sidebar half of Claude's browser tools on iOS, ported from the
/// desktop's ClaudeTabs (docs/design/claude.md, "Browser tools"): the ids,
/// list_tabs, move_tab, rename_tab, create_folder, split_tabs and
/// create_space, with the desktop's words. What needs the app (windows,
/// web views, sync) stays in the app's ClaudeTools, which says what's live.
///
/// Ids are the desktop's: `n` and the first 8 hex digits of an entry's
/// UUID (tabs, splits, folders), `s` and 8 for spaces. Entries keep their
/// UUIDs across synced devices, so they're the same ids as on the Mac. A
/// bare UUID prefix is taken too (what iOS listed before).
///
/// Incognito and the archive are never listed or found.
@MainActor
public struct SidebarTools {
  /// A tool's answer: its text, and whether it's an error.
  public struct Outcome: Equatable, Sendable {
    public var text: String
    public var isError: Bool
    public static func text(_ text: String) -> Outcome { Outcome(text: text, isError: false) }
    public static func error(_ text: String) -> Outcome { Outcome(text: text, isError: true) }
  }

  /// What the app knows of a tab beyond the sidebar.
  public struct TabState: Equatable, Sendable {
    /// The live page's title and URL; nil when unloaded (the entry's then).
    public var title: String?
    public var url: String?
    public var loaded = false
    /// Shown in a window.
    public var front = false
    public var target = false
    public var opened = false
    public var muted = false
    public var playing = false

    public init(
      title: String? = nil, url: String? = nil, loaded: Bool = false, front: Bool = false, target: Bool = false,
      opened: Bool = false, muted: Bool = false, playing: Bool = false
    ) {
      self.title = title
      self.url = url
      self.loaded = loaded
      self.front = front
      self.target = target
      self.opened = opened
      self.muted = muted
      self.playing = playing
    }
  }

  public static let idChars = 8

  public let store: SpaceStore
  /// The space the user's window (the chat's) shows.
  public var shownSpace: Space?
  /// Spaces other windows (iPad) show.
  public var otherWindowSpaces: Set<String> = []
  /// Which entries Claude sees: the phone's sidebar hides desktop-only
  /// pages, and so does Claude.
  public var isVisible: (SidebarNode) -> Bool = { _ in true }
  /// A profile's name, for other profiles' favorites.
  public var profileName: (String) -> String? = { _ in nil }

  public init(store: SpaceStore, shownSpace: Space?) {
    self.store = store
    self.shownSpace = shownSpace
  }

  // MARK: Ids

  public static func nodeID(_ node: SidebarNode) -> String { "n" + node.id.lowercased().prefix(idChars) }
  public static func spaceID(_ space: Space) -> String { "s" + space.id.lowercased().prefix(idChars) }

  /// Every entry Claude may use: the spaces' Pinned and Today (not
  /// incognito's, not the archive) and every profile's favorites.
  public var entries: [SidebarNode] {
    let roots = store.spaces.flatMap { [$0.pinned, $0.today] } + store.favoritesRoots
    return roots.flatMap { $0.children.flatMap(\.subtree) }.filter(isVisible)
  }

  /// The entry an id names: `n` and its UUID's start, or the start alone.
  public func node(_ id: String?) -> SidebarNode? {
    guard var id = id?.trimmingCharacters(in: .whitespaces).lowercased(), !id.isEmpty else { return nil }
    // UUIDs are hex: an "n" is the desktop's prefix.
    if id.hasPrefix("n") { id.removeFirst() }
    guard id.count >= 4 else { return nil }
    let matches = entries.filter { $0.id.lowercased().hasPrefix(id) }
    return matches.count == 1 ? matches[0] : nil
  }

  /// A space by its id (`s` and 8) or its name (any case).
  public func space(_ nameOrID: String?) -> Space? {
    guard let wanted = nameOrID?.trimmingCharacters(in: .whitespaces), !wanted.isEmpty else { return nil }
    let lower = wanted.lowercased()
    return store.spaces.first { Self.spaceID($0) == lower }
      ?? store.spaces.first { $0.name.lowercased() == lower }
  }

  /// The favorites root a node is under, if any.
  private func favoritesRoot(of node: SidebarNode) -> SidebarNode? {
    if case .root(.favorites) = node.root.kind { return node.root }
    return nil
  }

  // MARK: list_tabs

  /// The sidebar as the desktop lists it: Favorites, then each space with
  /// Pinned (folders and splits nested) and Today. Today is listed newest
  /// first, as the phone's sidebar shows it.
  public func listTabs(state: (SidebarNode) -> TabState) -> String {
    var text = ""
    func list(_ nodes: [SidebarNode], _ depth: Int) {
      let indent = String(repeating: "  ", count: depth)
      for node in nodes where isVisible(node) {
        switch node.kind {
        case .tab:
          let s = state(node)
          let url = s.url ?? node.url
          let title = node.customTitle.isEmpty ? (s.title.flatMap { $0.isEmpty ? nil : $0 } ?? node.displayTitle) : node.customTitle
          var line = "\(indent)- \(Self.nodeID(node)): \(title) — \(url)"
          if !s.loaded { line += " [unloaded]" }
          if s.front { line += " [front]" }
          if s.target { line += " [you act on this]" }
          if s.muted { line += " [muted]" }
          if s.playing { line += " [playing]" }
          if s.opened { line += " [you opened it]" }
          text += line + "\n"
        case .split:
          text += "\(indent)- split \(Self.nodeID(node)), side by side:\n"
          list(node.children, depth + 1)
        case .folder:
          text += "\(indent)- folder \"\(node.title)\" (folder_id \(Self.nodeID(node))):\n"
          list(node.children, depth + 1)
        case .root:
          break
        }
      }
    }
    for root in store.favoritesRoots where root.children.contains(where: isVisible) {
      if root === store.favorites {
        text += "Favorites (every space):\n"
      } else {
        let profile = String(root.id.dropFirst("favorites:".count))
        let name = profileName(profile).map { "“\($0)”" } ?? "another profile"
        text += "\(text.isEmpty ? "" : "\n")Favorites (every space of \(name)):\n"
      }
      list(root.children, 1)
    }
    for space in store.spaces {
      text += "\(text.isEmpty ? "" : "\n")Space \"\(space.name)\" (\(Self.spaceID(space)))"
      if space === shownSpace {
        text += " [shown in the user's window]"
      } else if otherWindowSpaces.contains(space.id) {
        text += " [shown in another window]"
      }
      text += ":\n  Pinned:\n"
      list(space.pinned.children, 2)
      text += "  Today:\n"
      list(space.today.children.reversed(), 2)
    }
    return text.isEmpty ? "No tabs." : text
  }

  // MARK: Organizing

  /// move_tab: reorder, pin, unpin, (un)favorite, into a folder, or to
  /// another space. `index` counts as the sidebar shows the new siblings
  /// (Today newest first); without it the entry goes last (Today: on top,
  /// as a new tab does).
  public func move(_ input: JSONValue) -> Outcome {
    guard let id = input["tab_id"]?.string, let to = input["to"]?.string else { return .error("Give tab_id and to.") }
    guard let node = self.node(id), node.isTab || node.isSplit else {
      return .error("No tab or split \(id) in the sidebar. list_tabs gives the ids.")
    }
    let parent: SidebarNode
    switch to {
    case "favorites":
      // The favorites of the entry's profile.
      parent = favoritesRoot(of: node) ?? store.favorites(for: store.space(of: node))
    case "folder":
      guard let folder = self.node(input["folder_id"]?.string), folder.isFolder else {
        return .error("Give the folder_id of a folder from list_tabs.")
      }
      parent = folder
    case "pinned", "today":
      let target: Space
      if let name = input["space"]?.string {
        guard let space = self.space(name) else { return .error("No space \"\(name)\".") }
        target = space
      } else if let own = store.space(of: node) {
        target = own
      } else {
        // Out of the favorites: into the user's window's space.
        guard let shown = shownSpace, !shown.isIncognito else { return .error("No window to take it to.") }
        target = shown
      }
      parent = to == "pinned" ? target.pinned : target.today
    default:
      return .error("to is pinned, today, favorites or folder.")
    }
    guard store.canMove(node, to: parent) else {
      return .error("That move isn't possible (a split can't go to Favorites, say).")
    }
    let siblings = parent.children.filter { $0 !== node }.count
    var index = siblings
    if let wanted = input["index"]?.number, wanted >= 0 {
      let shown = min(Int(wanted), siblings)
      index = parent === store.space(of: parent)?.today ? siblings - shown : shown
    }
    guard store.move(node, to: parent, at: index), parent.contains(node) else {
      return .error("The sidebar refused that move.")
    }
    return .text("Moved \(id).")
  }

  /// rename_tab: a tab's own name (empty: the page's title again), or a
  /// folder's name.
  public func rename(_ input: JSONValue) -> Outcome {
    guard let id = input["tab_id"]?.string, let node = self.node(id), let raw = input["title"]?.string else {
      return .error("Give the tab_id of a sidebar entry and a title.")
    }
    let title = raw.trimmingCharacters(in: .whitespacesAndNewlines)
    switch node.kind {
    case .folder:
      guard !title.isEmpty else { return .error("A folder needs a name.") }
    case .tab:
      break
    default:
      return .error("Rename one of the split's tabs.")
    }
    store.rename(node, customTitle: title)
    return .text("Renamed \(id).")
  }

  /// create_folder: in a space's Pinned (default) or Today, or in a folder.
  public func createFolder(_ input: JSONValue) -> Outcome {
    let name = input["name"]?.string?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    guard !name.isEmpty else { return .error("Give the folder a name.") }
    var space: Space
    if let wanted = input["space"]?.string {
      guard let found = self.space(wanted) else { return .error("No space \"\(wanted)\".") }
      space = found
    } else {
      guard let shown = shownSpace, !shown.isIncognito else { return .error("No window: name a space.") }
      space = shown
    }
    var parent = space.pinned
    switch input["in"]?.string {
    case nil, "pinned": break
    case "today": parent = space.today
    case let folderID:
      guard let folder = self.node(folderID), folder.isFolder, let folderSpace = store.space(of: folder) else {
        return .error("in is pinned, today, or a folder_id.")
      }
      parent = folder
      space = folderSpace
    }
    guard let folder = store.addFolder(name: name, to: parent) else {
      return .error("The folder couldn't be made there.")
    }
    return .text("Created folder \"\(name)\": folder_id \(Self.nodeID(folder)).")
  }

  /// split_tabs: `tab_id` joins `with_tab_id` side by side.
  public func split(_ input: JSONValue) -> Outcome {
    guard let id = input["tab_id"]?.string, let with = input["with_tab_id"]?.string,
      let node = self.node(id), let target = self.node(with)
    else { return .error("Give two tab_ids from list_tabs.") }
    guard let space = store.space(of: node), store.space(of: target) === space,
      store.split(target, with: node) != nil
    else {
      return .error("Those two can't be shown side by side (they must be tabs of the same space, not favorites).")
    }
    return .text("\(id) and \(with) are side by side now; activate_tab shows them.")
  }

  /// create_space: in the profile of the shown space.
  public func createSpace(_ input: JSONValue) -> (Outcome, Space?) {
    let name = input["name"]?.string?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
    guard !name.isEmpty else { return (.error("Give the space a name."), nil) }
    let profile = shownSpace.flatMap { $0.isIncognito ? nil : $0.profileUUID } ?? ""
    let space = store.addSpace(name: name, profile: profile)
    return (.text("Created space \"\(name)\" (\(Self.spaceID(space)))."), space)
  }

  // MARK: Media

  /// mute_tab's script, in Claude's content world (the DOM is shared):
  /// media the page plays is muted while `__compassMuted` holds, and the
  /// elements it muted are remembered, so unmuting leaves the page's own
  /// muted videos muted. Web Audio isn't reached.
  public static let muteScript = #"""
    (() => {
      globalThis.__compassMuted = true;
      if (!globalThis.__compassMutedBy) {
        const mutedBy = globalThis.__compassMutedBy = new Set();
        const mute = (m) => {
          if (!globalThis.__compassMuted || !(m instanceof HTMLMediaElement) || m.muted) return;
          mutedBy.add(m);
          m.muted = true;
        };
        globalThis.__compassMuteAll = () => document.querySelectorAll('audio, video').forEach(mute);
        for (const type of ['play', 'playing', 'volumechange', 'loadedmetadata']) {
          document.addEventListener(type, (e) => mute(e.target), true);
        }
        document.addEventListener('DOMContentLoaded', () => globalThis.__compassMuteAll());
      }
      globalThis.__compassMuteAll();
    })();
    """#

  public static let unmuteScript = #"""
    (() => {
      globalThis.__compassMuted = false;
      for (const m of globalThis.__compassMutedBy || []) m.muted = false;
      if (globalThis.__compassMutedBy) globalThis.__compassMutedBy.clear();
    })();
    """#
}

extension SidebarTools {
  // MARK: History and downloads

  /// A search_history line, as the desktop's.
  public static func historyLine(title: String, url: String, date: Date) -> String {
    "- \(title) — \(url) (\(date.formatted(date: .numeric, time: .shortened)))\n"
  }

  /// A list_downloads line, as the desktop's. `total` is nil when the size
  /// isn't known; `url` when the source isn't kept (finished files).
  public static func downloadLine(
    name: String, state: String, bytes: Int64, total: Int64?, url: String?, savedAt: String, date: Date
  ) -> String {
    let size = total.map { "\(bytes) of \($0) bytes" } ?? "\(bytes) bytes"
    let from = url.map { " from \($0)" } ?? ""
    return "- \(name) (\(state), \(size))\(from), saved at \(savedAt), \(date.formatted(date: .numeric, time: .shortened))\n"
  }
}
