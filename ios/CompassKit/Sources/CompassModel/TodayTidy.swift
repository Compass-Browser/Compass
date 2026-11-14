// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Tidy Today in the store (docs/design/ai-sidebar.md, "Tidy Today"; the
/// desktop's SpaceModel::TidyToday() and UndoTidy()): Today's loose entries
/// grouped into folders Claude named, and back, each in one change for sync.
///
/// Today's top is the end of its children here: the sidebar shows Today
/// newest first. So "the place of a group's topmost entry" is that of the
/// entry with the highest index, and a folder's entries go in the order the
/// sidebar showed them (topmost first).
extension SpaceStore {
  /// A folder to make, named `name`, of Today's loose entries `entries`
  /// (their IDs).
  public struct TidyGroup: Sendable, Equatable {
    public var name: String
    public var entries: [String]

    public init(name: String, entries: [String]) {
      self.name = name
      self.entries = entries
    }
  }

  /// What `undoTidy` needs: the space, Today's entries before (their IDs,
  /// in `today.children`'s order), and the folders made.
  public struct TodayTidy: Sendable, Equatable {
    public var spaceID: String
    public var before: [String]
    public var folders: [String]
  }

  /// The name a folder Tidy makes goes back to with Undo Name.
  public static let tidyFolderPlaceholder = "New Folder"

  /// Whether `node` is a loose Today entry Tidy may group: a tab or split
  /// at the top level of a Today that isn't incognito's.
  public func isLooseTodayEntry(_ node: SidebarNode?) -> Bool {
    guard let node, node.isTab || node.isSplit, let parent = node.parent, parent.kind == .root(.today),
      let space = space(of: node), !space.isIncognito, space.today === parent
    else { return false }
    return true
  }

  /// Puts each group's entries into a new Today folder named by Claude
  /// (`.claude`; Undo Name goes back to `tidyFolderPlaceholder`), at the
  /// place of its topmost entry, in the order the sidebar shows them. An ID
  /// that isn't a loose entry of `space`'s Today, or is in an earlier group,
  /// is skipped, and a group left with fewer than 2 entries (or no name)
  /// makes no folder. All in one batch, so sync sends one change. Folders and
  /// their contents, and Pinned, are left alone. Never in incognito.
  @discardableResult
  public func tidyToday(in space: Space, groups: [TidyGroup]) -> TodayTidy {
    let today = space.today
    var tidy = TodayTidy(spaceID: space.id, before: today.children.map(\.id), folders: [])
    guard !space.isIncognito, contains(today) else { return tidy }
    var taken: Set<String> = []
    batch {
      for group in groups {
        var entries: [SidebarNode] = []
        for id in group.entries where !taken.contains(id) {
          guard let entry = node(id: id), entry.parent === today, isLooseTodayEntry(entry) else { continue }
          taken.insert(id)
          entries.append(entry)
        }
        guard entries.count >= 2, !group.name.isEmpty else { continue }
        // Topmost first: the highest index.
        let index = { (node: SidebarNode) in today.children.firstIndex { $0 === node } ?? -1 }
        entries.sort { index($0) > index($1) }
        // Just above the topmost entry, which then leaves its place to it.
        guard let folder = addFolder(name: Self.tidyFolderPlaceholder, to: today, at: index(entries[0]) + 1)
        else { continue }
        setFolderName(folder, group.name, source: .claude)
        for (i, entry) in entries.enumerated() { move(entry, to: folder, at: i) }
        tidy.folders.append(folder.id)
      }
    }
    return tidy
  }

  /// Undoes `tidy`: the folders' contents go back into Today and the folders
  /// go (anything put in them since is left loose), then every entry from
  /// before that's still at the top of Today goes back to its place, with
  /// what came since above them (Today's top, where new entries go). One
  /// batch too. False, and nothing changes, if none of its folders is still
  /// in its space's Today.
  @discardableResult
  public func undoTidy(_ tidy: TodayTidy) -> Bool {
    guard let space = space(id: tidy.spaceID), !space.isIncognito else { return false }
    let today = space.today
    let folders = tidy.folders.compactMap { node(id: $0) }.filter { $0.isFolder && $0.parent === today }
    guard !folders.isEmpty else { return false }
    batch {
      for folder in folders {
        for child in folder.children {
          let at = today.children.firstIndex { $0 === folder } ?? today.children.count
          move(child, to: today, at: at)
        }
        // Emptied, a Today folder goes by itself; one left with something
        // that couldn't move stays.
        if folder.parent != nil, folder.children.isEmpty { remove(folder) }
      }
      let before = Set(tidy.before)
      var order = tidy.before.compactMap { id in node(id: id).flatMap { $0.parent === today ? $0 : nil } }
      order += today.children.filter { !before.contains($0.id) }
      for (i, entry) in order.enumerated() where today.children.firstIndex(where: { $0 === entry }) != i {
        move(entry, to: today, at: i)
      }
    }
    return true
  }
}
