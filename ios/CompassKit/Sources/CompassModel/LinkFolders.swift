// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Link folders on iOS (docs/design/sidebar.md, "Link folders"; the
/// desktop's TabStripBridge::GetLinkFolderPlace()): links opened in the
/// background from a page (the link's long-press "Open in Background", or
/// ⌘-click on iPad) gather with that page in a Today folder, so a page's tabs
/// stay together.
///
///  - The opener in Today, not in a folder: a folder named after it takes
///    its place, holding the opener and then the link. A split opener goes
///    in whole.
///  - The opener in a Today folder (made this way or by hand): the link
///    joins that folder.
///  - The opener pinned, a favorite, or in another space: it stays put; its
///    links gather in a folder at the top of Today, named after it, which
///    later links reuse while it's there.
///  - Within the folder, a link goes after the one the opener opened before
///    (if it's still there), else right after the opener (at the end, for a
///    folder at the top of Today).
///
/// A folder made this way is marked as this device's link folder
/// (`SpaceStore.markLinkFolder`), which Claude may name.
///
/// One per window, as the desktop's `link_folders_`; not saved.
@MainActor
public struct LinkFolders {
  /// The folders at the top of Today for pinned entries and favorites, by
  /// the opener's ID.
  var folders: [String: String] = [:]
  /// The link each opener opened last, by the opener's ID.
  var lastLink: [String: String] = [:]

  public init() {}

  /// A link folder's name: its opener's (a split's first pane's) name or
  /// title, else its host.
  public static func title(of opener: SidebarNode) -> String {
    let tab = opener.isSplit ? opener.children.first ?? opener : opener
    if !tab.customTitle.isEmpty { return tab.customTitle }
    if !tab.title.isEmpty { return tab.title }
    if let host = URL(string: tab.url)?.host(), !host.isEmpty { return host }
    return String(localized: "Links", bundle: .module, comment: "Sidebar: name of a link folder (links opened from one page) whose page has no title or host.")
  }

  /// Adds a tab for `url`, opened in the background from the tab `opener`,
  /// to `space`'s Today, in its link folder (made if needed). `space` is the
  /// one the link opens in: the opener's, or for a favorite the window's.
  /// Nil if it can't be added.
  @discardableResult
  public mutating func addLink(
    url: String, from opener: SidebarNode, in space: Space, store: SpaceStore, now: Date = Date()
  ) -> SidebarNode? {
    // A split's pane opens next to its split.
    let entry = opener.parent?.isSplit == true ? opener.parent! : opener
    let today = space.today
    var tab: SidebarNode?
    store.batch {
      let folder: SidebarNode
      var index: Int
      if today.contains(entry), let parent = entry.parent {
        if parent.isFolder {
          folder = parent
        } else {
          // The opener's own folder takes its place.
          let at = today.children.firstIndex { $0 === entry } ?? today.children.count
          guard let made = store.addFolder(name: Self.title(of: entry), to: today, at: at) else { return }
          store.markLinkFolder(made)
          guard store.move(entry, to: made, at: 0, now: now) else {
            store.remove(made)
            return
          }
          folder = made
        }
        index = (folder.children.firstIndex { $0 === entry } ?? -1) + 1
      } else {
        // Pinned entries and favorites stay put; their links gather in a
        // folder at the top of Today (the end: Today shows newest first).
        if let id = folders[opener.id], let existing = store.node(id: id), existing.isFolder,
          existing.parent != nil, today.contains(existing)
        {
          folder = existing
        } else {
          guard let made = store.addFolder(name: Self.title(of: entry), to: today, at: today.children.count)
          else { return }
          store.markLinkFolder(made)
          folders[opener.id] = made.id
          folder = made
        }
        index = folder.children.count
      }
      // After the link the opener opened before, if it's still here.
      if let id = lastLink[opener.id], let i = folder.children.firstIndex(where: { $0.id == id }) {
        index = i + 1
      }
      tab = store.addTab(url: url, to: folder, at: index, now: now)
    }
    if let tab { lastLink[opener.id] = tab.id }
    return tab
  }
}
