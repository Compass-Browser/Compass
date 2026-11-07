// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Observation

/// The entries the user closed, newest last, so that undo (shake, the toast,
/// ⌘⇧T) puts them back and redo closes them again. The phone's side of the
/// desktop's ClosedEntries (docs/design/sidebar.md, "Undo closing"). A step
/// keeps each entry's node itself (a removed one detached, with its subtree
/// and IDs) and its place: putting a node back under its old ID revives it
/// on the sync server (sync.md, "Create over a tombstone"). In memory only.
@MainActor
@Observable
public final class ClosedEntries {
  /// At most this many steps are kept; older ones are dropped.
  public static let maxSteps = 25

  /// How an entry closed, which redo does again.
  public enum Close: Sendable, Equatable {
    /// Gone from the sidebar (a Today row's ×, Delete, Remove from Favorites).
    case remove
    /// A Today entry moved to the archive (⌘W, a flicked card, Clear).
    case archive
    /// A tab that stays in the sidebar unloaded: a pinned tab or favorite,
    /// back on its pinned URL, or a split's pane.
    case unload
  }

  /// One thing that closed, and where it was.
  public struct Entry {
    public let node: SidebarNode
    public let close: Close
    /// Its place: the parent, the sibling before it (nil if it was first)
    /// and its index. Weak: a parent gone since sends it to its section's
    /// root.
    weak var parent: SidebarNode?
    weak var previous: SidebarNode?
    let index: Int
    /// A pane whose split of two ended as it left: the other pane.
    weak var splitSibling: SidebarNode?
    let section: Section?
    let spaceID: String?
    let profile: String
    let incognito: Bool
    /// `.unload`: the page the tab showed before it went back to its pinned
    /// URL.
    let url: String
    let title: String

    /// Captures `node`'s place; call it before closing it.
    @MainActor public init(_ node: SidebarNode, close: Close, in store: SpaceStore) {
      self.node = node
      self.close = close
      let parent = node.parent
      self.parent = parent
      index = parent?.children.firstIndex { $0 === node } ?? 0
      previous = index > 0 ? parent?.children[index - 1] : nil
      if let parent, parent.isSplit, parent.children.count == 2 {
        splitSibling = parent.children[index == 0 ? 1 : 0]
      }
      section = node.section
      spaceID = store.space(of: node)?.id
      profile = store.profile(of: node)
      incognito = store.isIncognito(node)
      url = node.url
      title = node.title
    }
  }

  /// What one user action closed.
  public struct Step {
    public var entries: [Entry] = []
    /// The space it closed in: undo goes back to it.
    public var spaceID: String?
    /// The tab that opens again on undo: the one shown as it closed, else
    /// the first that was open. The others come back unloaded.
    public var reopenID: String?
    /// Open tabs' session history (`WKWebView.interactionState`), by tab ID.
    public var states: [String: Any] = [:]

    public init() {}

    /// For the toast and the shake alert: the entry's title in quotes, or
    /// how many tabs.
    public var title: String {
      if entries.count == 1, let node = entries.first?.node, !node.isSplit { return "“\(node.displayTitle)”" }
      let tabs = entries.reduce(0) { $0 + max($1.node.tabs.count, 1) }
      return tabs == 1 ? "1 tab" : "\(tabs) tabs"
    }
  }

  public private(set) var undoSteps: [Step] = []
  public private(set) var redoSteps: [Step] = []

  public init() {}

  public var canUndo: Bool { !undoSteps.isEmpty }
  public var canRedo: Bool { !redoSteps.isEmpty }

  /// A new step. Drops the redos, unless it's a redo's (`keepRedo`).
  public func push(_ step: Step, keepRedo: Bool = false) {
    guard !step.entries.isEmpty else { return }
    if !keepRedo { redoSteps.removeAll() }
    undoSteps.append(step)
    if undoSteps.count > Self.maxSteps { undoSteps.removeFirst() }
  }

  public func takeUndo() -> Step? { undoSteps.popLast() }

  public func pushRedo(_ step: Step) {
    redoSteps.append(step)
    if redoSteps.count > Self.maxSteps { redoSteps.removeFirst() }
  }

  public func takeRedo() -> Step? { redoSteps.popLast() }

  /// Incognito keeps nothing: once its tabs are closed, none come back.
  public func dropIncognito() {
    undoSteps.removeAll { $0.entries.contains(where: \.incognito) }
    redoSteps.removeAll { $0.entries.contains(where: \.incognito) }
  }

  /// Puts `step`'s entries back in `store`, last closed first, so each
  /// finds the sibling it followed. A removed entry comes back under its
  /// IDs, an archived one moves back from the archive, and a pinned tab or
  /// favorite that stayed goes back to the page it showed (unless `isOpen`,
  /// showing another). Returns the tab to show, if it's there.
  @discardableResult
  public static func restore(
    _ step: Step, in store: SpaceStore, isOpen: (SidebarNode) -> Bool = { _ in false }, now: Date = Date()
  ) -> SidebarNode? {
    store.batch {
      for entry in step.entries.reversed() {
        restore(entry, in: store, isOpen: isOpen, now: now)
      }
    }
    guard let id = step.reopenID, let tab = store.node(id: id), tab.isTab, tab.section != .archive else { return nil }
    return tab
  }

  private static func restore(_ entry: Entry, in store: SpaceStore, isOpen: (SidebarNode) -> Bool, now: Date) {
    // By ID: sync may have put another node there meanwhile.
    let current = store.node(id: entry.node.id)
    switch entry.close {
    case .unload:
      guard let tab = current, tab.isTab, !entry.url.isEmpty, !isOpen(tab) else { return }
      store.update(tab, url: entry.url, title: entry.title)
      return
    case .archive, .remove:
      if let current {
        // Still archived: back from there. Anywhere else, it's back already.
        guard current.section == .archive, current.section != entry.section else { return }
        let (parent, index) = place(of: entry, in: store)
        if store.move(current, to: parent, at: index, now: now) { resplit(current, entry, in: store) }
        return
      }
      // Removed: the node itself, detached from whatever it was in since.
      let node = entry.node
      if node.parent != nil { store.rawDetach(node) }
      let (parent, index) = place(of: entry, in: store)
      if store.reinsert(node, into: parent, at: index, now: now) { resplit(node, entry, in: store) }
    }
  }

  /// Where an entry goes back: after the sibling it followed (or first, if
  /// it was), in its parent if that's still there; next to the other pane
  /// of a split that ended; else the top of its space's Today, the end of
  /// its Pinned, or its favorites.
  private static func place(of entry: Entry, in store: SpaceStore) -> (SidebarNode, Int) {
    if let parent = entry.parent, store.contains(parent), store.canContain(parent, entry.node.kind) {
      let index: Int
      if let previous = entry.previous, previous.parent === parent,
        let at = parent.children.firstIndex(where: { $0 === previous })
      {
        index = at + 1
      } else if entry.previous == nil {
        index = 0
      } else {
        index = min(entry.index, parent.children.count)
      }
      return (parent, index)
    }
    if let sibling = entry.splitSibling, store.contains(sibling), let parent = sibling.parent, !parent.isSplit,
      let at = parent.children.firstIndex(where: { $0 === sibling })
    {
      return (parent, at + (entry.index == 0 ? 0 : 1))
    }
    let space = entry.incognito ? store.incognito : entry.spaceID.flatMap(store.space(id:))
    guard let space, entry.section != .favorites else {
      let root = store.favorites(forProfile: entry.profile)
      return (root, root.children.count)
    }
    if entry.section == .pinned { return (space.pinned, space.pinned.children.count) }
    return (space.today, 0)
  }

  /// A pane put back next to the other pane of its split: split again, in
  /// the order they had.
  private static func resplit(_ node: SidebarNode, _ entry: Entry, in store: SpaceStore) {
    guard let sibling = entry.splitSibling, node.isTab, sibling.isTab, sibling.parent === node.parent,
      node.parent?.isSplit == false
    else { return }
    if entry.index == 0 { store.split(node, with: sibling) } else { store.split(sibling, with: node) }
  }
}
