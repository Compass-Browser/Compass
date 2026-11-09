// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Observation

/// What changed locally, for sync: entity IDs (space and node UUIDs) to put
/// or delete. Remote changes applied through `applyingRemote` aren't reported.
public struct SpaceStoreChange: Sendable, Equatable {
  public var putSpaces: Set<String> = []
  public var putNodes: Set<String> = []
  public var deletedSpaces: Set<String> = []
  public var deletedNodes: Set<String> = []
  /// Today entries whose URL, title or last-active time changed (sync
  /// throttles these; everything else goes at once).
  public var todayChurn: Set<String> = []

  public var isEmpty: Bool {
    putSpaces.isEmpty && putNodes.isEmpty && deletedSpaces.isEmpty && deletedNodes.isEmpty
      && todayChurn.isEmpty
  }
}

/// The browser-wide sidebar: spaces in order, and the favorites. A port of
/// the desktop's SpaceList + SpaceModel, with their invariants:
///  - folders only in Pinned and Today (they nest);
///  - splits hold only tabs, at most `maxSplitPanes`; a split left with one
///    pane is replaced by that pane;
///  - favorites hold only tabs;
///  - section roots can't move;
///  - a Today folder whose last child leaves (moved, removed, archived) is
///    removed too, and its emptied parents after it; one made empty stays.
///    Not while applying remote changes, whose order can empty a folder for
///    a moment.
@MainActor
@Observable
public final class SpaceStore {
  public static let maxSplitPanes = 4

  public private(set) var spaces: [Space] = []
  /// Incognito (docs/design/ios.md, "Incognito"): one space kept apart from
  /// `spaces`, so nothing that goes through them (saving, sync, sessions,
  /// widgets, Spotlight) sees it, and lookups (`node(id:)`, `space(id:)`,
  /// `space(of:)`) do. Its local changes are never reported; its entries
  /// can't move to or from other spaces; archiving one of them closes it.
  public let incognito = Space(id: "incognito", name: "Incognito", theme: SpaceTheme(color: nil, appearance: .dark), archiveAfter: nil, isIncognito: true)
  /// Every node ID ever seen under incognito, so deletions are dropped too.
  @ObservationIgnored private var incognitoIDs: Set<String> = []
  /// The home profile's favorites. Every profile has its own, as on the
  /// desktop (each profile's SpaceList has them); see `favorites(forProfile:)`.
  public let favorites = SidebarNode(id: "favorites", kind: .root(.favorites))
  /// The other profiles' favorites, by profile UUID (root ID
  /// "favorites:<uuid>").
  public private(set) var profileFavorites: [String: SidebarNode] = [:]
  /// The home profile's sync UUID once the chain's profile list gives it;
  /// empty before. A space with an empty profile or this one is the home's.
  public var homeProfileUUID = ""

  /// Called after each local change (not for remote ones).
  @ObservationIgnored public var onLocalChange: ((SpaceStoreChange) -> Void)?
  /// The position suffix for a new or moved entity, by its UUID. Sync sets
  /// it (Chromium derives suffixes from client tag hashes); random otherwise.
  @ObservationIgnored public var suffixForID: (String) -> [UInt8] = { _ in UniquePosition.randomSuffix() }

  @ObservationIgnored private var pending = SpaceStoreChange()
  @ObservationIgnored private var remoteDepth = 0
  @ObservationIgnored private var batchDepth = 0

  public init() {}

  // MARK: Lookup

  public func space(id: String) -> Space? { id == incognito.id ? incognito : spaces.first { $0.id == id } }

  public func node(id: String) -> SidebarNode? {
    for root in favoritesRoots {
      if let n = root.children.first(where: { $0.id == id }) { return n }
    }
    for space in spaces + [incognito] {
      if let n = space.allNodes.first(where: { $0.id == id }) { return n }
    }
    return nil
  }

  /// The space a node belongs to (nil for favorites).
  public func space(of node: SidebarNode) -> Space? {
    let root = node.root
    return (spaces + [incognito]).first { $0.pinned === root || $0.today === root || $0.archive === root }
  }

  /// Whether a node is in the incognito space.
  public func isIncognito(_ node: SidebarNode) -> Bool {
    let root = node.root
    return root === incognito.pinned || root === incognito.today || root === incognito.archive
  }

  public var allNodes: [SidebarNode] {
    favoritesRoots.flatMap { $0.children.flatMap(\.subtree) } + spaces.flatMap(\.allNodes)
  }

  // MARK: Profiles (docs/design/ios.md, "M8")

  /// Whether `profile` (a space's profile UUID) is the home profile.
  public func isHome(_ profile: String) -> Bool { profile.isEmpty || profile == homeProfileUUID }

  /// The home's favorites root, then the other profiles' by UUID.
  public var favoritesRoots: [SidebarNode] {
    [favorites] + profileFavorites.keys.sorted().map { profileFavorites[$0]! }
  }

  /// A profile's favorites root (made when first asked for).
  public func favorites(forProfile profile: String) -> SidebarNode {
    if isHome(profile) { return favorites }
    if let root = profileFavorites[profile] { return root }
    let root = SidebarNode(id: "favorites:" + profile, kind: .root(.favorites))
    profileFavorites[profile] = root
    return root
  }

  /// Whether two spaces' profile UUIDs name the same profile.
  public func sameProfile(_ a: String, _ b: String) -> Bool { isHome(a) ? isHome(b) : a == b }

  /// The spaces of a space's profile (itself included), in order.
  public func spaces(inProfileOf space: Space) -> [Space] {
    spaces.filter { sameProfile($0.profileUUID, space.profileUUID) }
  }

  /// What the command bar offers for a space (M8: scoped to its profile):
  /// the tabs of its profile's spaces (not archived) and its favorites.
  public func tabs(inProfileOf space: Space?) -> [SidebarNode] {
    let inSpaces = (space.map(spaces(inProfileOf:)) ?? spaces).flatMap { [$0.pinned, $0.today] }
    return (inSpaces + [favorites(for: space)]).flatMap { $0.children.flatMap(\.subtree) }.filter(\.isTab)
  }

  /// The favorites shown with a space: its profile's.
  public func favorites(for space: Space?) -> SidebarNode { favorites(forProfile: space?.profileUUID ?? "") }

  /// The profile a node belongs to: its space's, or its favorites root's.
  public func profile(of node: SidebarNode) -> String {
    let root = node.root
    if root === favorites { return homeProfileUUID }
    if case .root(.favorites) = root.kind { return String(root.id.dropFirst("favorites:".count)) }
    return space(of: node)?.profileUUID ?? ""
  }

  /// Moves a space to another profile (the desktop's MoveSpaceToProfile):
  /// same ID and entries; sync deletes it from the old profile's account
  /// and creates it in the new one's.
  public func setProfile(_ space: Space, to profile: String) {
    guard space.profileUUID != profile else { return }
    space.profileUUID = profile
    record {
      $0.putSpaces.insert(space.id)
      $0.putNodes.formUnion(space.allNodes.map(\.id))
    }
  }

  // MARK: Invariants (SpaceModel::CanContain / CanMove)

  public func canContain(_ parent: SidebarNode, _ kind: SidebarNode.Kind) -> Bool {
    switch (parent.kind, kind) {
    case (_, .root): false
    case (.root(.favorites), .tab): true
    case (.root(.favorites), _): false
    // Folders nest, in Pinned and in Today (the desktop's roots accept
    // folders in both).
    case (.root(.pinned), _), (.root(.today), _): true
    case (.folder, _): parent.section == .pinned || parent.section == .today
    case (.root(.archive), .folder): false
    case (.root(.archive), _): true
    case (.split, .tab): parent.children.count < Self.maxSplitPanes
    case (.split, _): false
    case (.tab, _): false
    }
  }

  public func canMove(_ node: SidebarNode, to parent: SidebarNode) -> Bool {
    if node.isRoot || node.contains(parent) { return false }
    // Incognito entries stay incognito, and nothing else goes in.
    if isIncognito(node) != (parent.root === incognito.pinned || parent.root === incognito.today
      || parent.root === incognito.archive) { return false }
    if node.parent === parent { return true }
    // No folder in the archive or the favorites.
    if node.isFolder, parent.section != .pinned && parent.section != .today { return false }
    return canContain(parent, node.kind)
  }

  // MARK: Spaces

  @discardableResult
  public func addSpace(
    name: String, theme: SpaceTheme = .blue, id: String = UUID().uuidString.lowercased(), profile: String = ""
  ) -> Space {
    let position = UniquePosition.forInsert(
      after: spaces.last?.position, before: nil, suffix: suffixForID(id))
    let space = Space(id: id, name: name, theme: theme, profileUUID: profile, position: position)
    spaces.append(space)
    record { $0.putSpaces.insert(space.id) }
    return space
  }

  public func renameSpace(_ space: Space, to name: String) {
    guard space.name != name else { return }
    space.name = name
    record { $0.putSpaces.insert(space.id) }
  }

  public func setTheme(_ space: Space, _ theme: SpaceTheme) {
    guard space.theme != theme else { return }
    space.theme = theme
    record { $0.putSpaces.insert(space.id) }
  }

  public func setArchiveAfter(_ space: Space, _ interval: TimeInterval?) {
    space.archiveAfter = interval
    record { $0.putSpaces.insert(space.id) }
  }

  public func moveSpace(_ space: Space, to index: Int) {
    guard let from = spaces.firstIndex(where: { $0 === space }) else { return }
    var others = spaces
    others.remove(at: from)
    let index = max(0, min(index, others.count))
    space.position = UniquePosition.forInsert(
      after: index > 0 ? others[index - 1].position : nil,
      before: index < others.count ? others[index].position : nil, suffix: suffixForID(space.id))
    others.insert(space, at: index)
    spaces = others
    record { $0.putSpaces.insert(space.id) }
  }

  /// Deletes a space and everything in it.
  public func deleteSpace(_ space: Space) {
    let nodes = space.allNodes.map(\.id)
    spaces.removeAll { $0 === space }
    record {
      $0.deletedSpaces.insert(space.id)
      $0.deletedNodes.formUnion(nodes)
    }
  }

  // MARK: Nodes

  /// Adds a tab to a section root, folder or split, at `index` among its
  /// siblings (default: the end; Today's newest-first order is the UI's).
  @discardableResult
  public func addTab(
    url: String, title: String = "", to parent: SidebarNode, at index: Int? = nil,
    id: String = UUID().uuidString.lowercased(), now: Date = Date()
  ) -> SidebarNode? {
    let node = SidebarNode(id: id, kind: .tab, title: title, url: url)
    if parent.section == .today { node.lastActive = now }
    return add(node, to: parent, at: index)
  }

  @discardableResult
  public func addFolder(
    name: String, to parent: SidebarNode, at index: Int? = nil, id: String = UUID().uuidString.lowercased()
  ) -> SidebarNode? {
    add(SidebarNode(id: id, kind: .folder, title: name), to: parent, at: index)
  }

  private func add(_ node: SidebarNode, to parent: SidebarNode, at index: Int?) -> SidebarNode? {
    guard canContain(parent, node.kind) else { return nil }
    node.position = position(in: parent, at: index ?? parent.children.count, for: node.id)
    parent.insert(node)
    updateSavedURLs(node)
    record { $0.putNodes.insert(node.id) }
    return node
  }

  /// Moves a node to `parent` at `index` (counted without the node itself).
  @discardableResult
  public func move(_ node: SidebarNode, to parent: SidebarNode, at index: Int, now: Date = Date()) -> Bool {
    guard canMove(node, to: parent), let oldParent = node.parent else { return false }
    oldParent.remove(node)
    node.position = position(in: parent, at: index, for: node.id)
    let enteringToday = parent.section == .today && oldParent.section != .today
    let leavingArchive = oldParent.section == .archive && parent.section != .archive
    if enteringToday { node.lastActive = now }
    if leavingArchive { node.archivedAt = nil }
    if parent.section == .archive && oldParent.section != .archive { node.archivedAt = now }
    parent.insert(node)
    updateSavedURLs(node)
    let elsewhere = oldParent.root !== parent.root
    record { change in
      // A move to another section or space moves the whole subtree (its
      // section and space fields).
      change.putNodes.formUnion(elsewhere ? node.subtree.map(\.id) : [node.id])
    }
    normalizeSplit(oldParent)
    if oldParent !== parent { closeIfEmptyTodayFolder(oldParent) }
    return true
  }

  /// Moves a node to another space's root of the same section.
  public func move(_ node: SidebarNode, toSpace target: Space, section: Section, at index: Int = 0) -> Bool {
    guard let root = target.root(section) else { return false }
    return move(node, to: root, at: index)
  }

  /// Removes a node and its subtree.
  public func remove(_ node: SidebarNode) {
    guard !node.isRoot, let parent = node.parent else { return }
    let ids = node.subtree.map(\.id)
    if isIncognito(node) { incognitoIDs.formUnion(ids) }
    parent.remove(node)
    record { $0.deletedNodes.formUnion(ids) }
    normalizeSplit(parent)
    closeIfEmptyTodayFolder(parent)
  }

  /// Puts back a node `remove` took out, with its subtree and IDs (undoing
  /// a close, ClosedEntries): sync creates it again over its tombstone.
  @discardableResult
  public func reinsert(_ node: SidebarNode, into parent: SidebarNode, at index: Int, now: Date = Date()) -> Bool {
    guard node.parent == nil, !node.isRoot, contains(parent), canContain(parent, node.kind) else { return false }
    // Nothing goes in or out of incognito.
    guard incognitoIDs.contains(node.id) == isIncognito(parent) else { return false }
    node.position = position(in: parent, at: index, for: node.id)
    if parent.section == .today {
      node.lastActive = now
      node.archivedAt = nil
    }
    parent.insert(node)
    updateSavedURLs(node)
    record { $0.putNodes.formUnion(node.subtree.map(\.id)) }
    return true
  }

  /// Whether `node` is in the sidebar: under a space's or favorites' root.
  public func contains(_ node: SidebarNode) -> Bool {
    let root = node.root
    guard root.isRoot else { return false }
    return favoritesRoots.contains { $0 === root } || (spaces + [incognito]).contains { space in
      space.pinned === root || space.today === root || space.archive === root
    }
  }

  /// Removes `parent` if it's a Today folder left empty (`remove` goes on up).
  private func closeIfEmptyTodayFolder(_ parent: SidebarNode) {
    guard remoteDepth == 0, parent.isFolder, parent.children.isEmpty, parent.section == .today else { return }
    remove(parent)
  }

  public func rename(_ node: SidebarNode, customTitle: String) {
    if node.isFolder {
      guard node.title != customTitle else { return }
      node.title = customTitle
    } else {
      guard node.customTitle != customTitle else { return }
      node.customTitle = customTitle
    }
    record { $0.putNodes.insert(node.id) }
  }

  public func setCollapsed(_ node: SidebarNode, _ collapsed: Bool) {
    guard node.isFolder, node.collapsed != collapsed else { return }
    node.collapsed = collapsed
    record { $0.putNodes.insert(node.id) }
  }

  /// A navigation or title change in a tab.
  public func update(_ node: SidebarNode, url: String? = nil, title: String? = nil) {
    var changed = false
    if let url, url != node.url {
      node.url = url
      changed = true
    }
    if let title, title != node.title {
      node.title = title
      changed = true
    }
    // A pinned tab added before it had a page takes its first one.
    if node.isTab, node.savedURL.isEmpty, !node.url.isEmpty, node.keepsSavedURL == true {
      node.savedURL = node.url
      node.savedTitle = node.title
      changed = true
    }
    guard changed else { return }
    record {
      if node.section == .today { $0.todayChurn.insert(node.id) } else { $0.putNodes.insert(node.id) }
    }
  }

  /// Sets the page a pinned tab or favorite goes back to ("Use This Page as
  /// Pinned URL"). Tabs outside Pinned and Favorites keep none.
  public func setSavedURL(_ node: SidebarNode, url: String, title: String) {
    guard node.isTab, node.keepsSavedURL == true, !url.isEmpty,
      node.savedURL != url || node.savedTitle != title
    else { return }
    node.savedURL = url
    node.savedTitle = title
    record { $0.putNodes.insert(node.id) }
  }

  /// Points an unloaded pinned tab or favorite back at its pinned URL and
  /// title: the user closed it, or chose "Back to Pinned URL" while it
  /// wasn't loaded. A live tab navigates instead (the app's job).
  public func goBackToSavedURL(_ node: SidebarNode) {
    guard node.isAwayFromSavedURL else { return }
    update(node, url: node.savedURL, title: node.savedTitle)
  }

  /// Gives the tabs of `node`'s subtree a pinned URL if it's in Pinned or
  /// Favorites and they have none, or clears theirs if it's elsewhere (the
  /// desktop's SpaceModel::UpdateSavedUrls). Not reported: it happens as the
  /// subtree is added or moved, which is. A detached node is left alone.
  func updateSavedURLs(_ node: SidebarNode) {
    guard let keeps = node.keepsSavedURL else { return }
    for tab in node.subtree where tab.isTab {
      if !keeps {
        tab.savedURL = ""
        tab.savedTitle = ""
      } else if tab.savedURL.isEmpty {
        tab.savedURL = tab.url
        tab.savedTitle = tab.title
      }
    }
  }

  /// The user looked at a Today tab.
  public func touch(_ node: SidebarNode, now: Date = Date()) {
    guard node.section == .today else { return }
    let entry = node.parent?.isSplit == true ? node.parent! : node
    entry.lastActive = now
    if entry !== node { node.lastActive = now }
    record { $0.todayChurn.insert(entry.id) }
  }

  /// Makes `tab` a pane of `target` (a tab, which becomes a split, or a split).
  @discardableResult
  public func split(_ target: SidebarNode, with tab: SidebarNode) -> SidebarNode? {
    guard tab.isTab, target !== tab, let targetParent = target.parent, targetParent.section != .favorites
    else { return nil }
    if target.isSplit {
      guard target.children.count < Self.maxSplitPanes else { return nil }
      return move(tab, to: target, at: target.children.count) ? target : nil
    }
    guard target.isTab, !targetParent.isSplit else { return nil }
    let split = SidebarNode(kind: .split, lastActive: target.lastActive)
    split.position = target.position
    batch {
      targetParent.remove(target)
      targetParent.insert(split)
      record { $0.putNodes.insert(split.id) }
      target.position = position(in: split, at: 0, for: target.id)
      split.insert(target)
      record { $0.putNodes.insert(target.id) }
      _ = move(tab, to: split, at: 1)
    }
    return split
  }

  /// Archives a Today entry (or several), as the desktop: a move to ARCHIVE.
  public func archive(_ node: SidebarNode, now: Date = Date()) {
    guard let space = space(of: node), node.parent === space.today else { return }
    // Incognito keeps nothing: a closed entry is gone.
    if space.isIncognito {
      remove(node)
      return
    }
    move(node, to: space.archive, at: 0, now: now)
  }

  /// Brings an archived entry back to Today under fresh IDs, as the desktop
  /// does (SpaceModel::Unarchive): the archived entities are deleted and new
  /// Today entries made, so no device re-creates a deleted UUID.
  @discardableResult
  public func restore(_ entry: SidebarNode, now: Date = Date()) -> SidebarNode? {
    guard let space = space(of: entry), entry.parent === space.archive else { return nil }
    var result: SidebarNode?
    batch {
      let tabs = entry.tabs
      remove(entry)
      guard let first = tabs.first,
        let restored = addTab(url: first.url, title: first.title, to: space.today, at: 0, now: now)
      else { return }
      result = restored
      if tabs.count > 1, let second = addTab(url: tabs[1].url, title: tabs[1].title, to: space.today, now: now) {
        result = split(restored, with: second)
      }
    }
    return result
  }

  /// Today entries unseen for longer than their space's delay go to the archive.
  @discardableResult
  public func archiveIdleEntries(now: Date = Date()) -> Int {
    var count = 0
    batch {
      for space in spaces {
        guard let after = space.archiveAfter, after > 0 else { continue }
        for entry in space.today.children {
          guard let seen = entry.lastActive, now.timeIntervalSince(seen) > after else { continue }
          archive(entry, now: now)
          count += 1
        }
      }
    }
    return count
  }

  /// Entries archived longer ago than `age` are dropped (the 30-day cap).
  public func pruneArchive(olderThan age: TimeInterval = 30 * 24 * 60 * 60, now: Date = Date()) {
    batch {
      for space in spaces {
        for entry in space.archive.children {
          if let at = entry.archivedAt, now.timeIntervalSince(at) > age { remove(entry) }
        }
      }
    }
  }

  /// A split left with one pane is replaced by that pane, in its position.
  private func normalizeSplit(_ split: SidebarNode) {
    guard split.isSplit, let parent = split.parent else { return }
    if split.children.isEmpty {
      parent.remove(split)
      record { $0.deletedNodes.insert(split.id) }
      closeIfEmptyTodayFolder(parent)
    } else if split.children.count == 1 {
      let pane = split.children[0]
      split.remove(pane)
      parent.remove(split)
      pane.position = split.position
      pane.lastActive = pane.lastActive ?? split.lastActive
      parent.insert(pane)
      record {
        $0.deletedNodes.insert(split.id)
        $0.putNodes.insert(pane.id)
      }
    }
  }

  // MARK: Positions

  func position(in parent: SidebarNode, at index: Int, for id: String) -> UniquePosition {
    let siblings = parent.children.filter { $0.id != id }
    let index = max(0, min(index, siblings.count))
    return UniquePosition.forInsert(
      after: index > 0 ? siblings[index - 1].position : nil,
      before: index < siblings.count ? siblings[index].position : nil, suffix: suffixForID(id))
  }

  // MARK: Change tracking

  /// Groups several changes into one notification.
  public func batch(_ body: () -> Void) {
    batchDepth += 1
    body()
    batchDepth -= 1
    flush()
  }

  /// Runs remote changes without reporting them back.
  public func applyingRemote(_ body: () -> Void) {
    remoteDepth += 1
    body()
    remoteDepth -= 1
  }

  public var isApplyingRemote: Bool { remoteDepth > 0 }

  private func record(_ body: (inout SpaceStoreChange) -> Void) {
    guard remoteDepth == 0 else { return }
    body(&pending)
    flush()
  }

  private func flush() {
    guard batchDepth == 0, !pending.isEmpty else { return }
    var change = pending
    pending = SpaceStoreChange()
    // Incognito never reaches sync.
    incognitoIDs.formUnion(incognito.allNodes.map(\.id))
    if !incognitoIDs.isEmpty || change.putSpaces.contains(incognito.id) || change.deletedSpaces.contains(incognito.id) {
      change.putNodes.subtract(incognitoIDs)
      change.deletedNodes.subtract(incognitoIDs)
      change.todayChurn.subtract(incognitoIDs)
      change.putSpaces.remove(incognito.id)
      change.deletedSpaces.remove(incognito.id)
      guard !change.isEmpty else { return }
    }
    onLocalChange?(change)
  }

  // MARK: Raw access for loading and sync (no invariants checked, no
  // change reported). Callers must keep the tree valid.

  public func rawInsertSpace(_ space: Space) {
    spaces.append(space)
    spaces.sort { $0.position < $1.position }
  }

  public func rawRemoveSpace(_ space: Space) { spaces.removeAll { $0 === space } }

  public func rawSortSpaces() { spaces.sort { $0.position < $1.position } }

  /// Also gives the node's tabs a pinned URL or clears it, as a move would.
  public func rawInsert(_ node: SidebarNode, into parent: SidebarNode) {
    parent.insert(node)
    updateSavedURLs(node)
  }

  /// Sets a pinned tab's or favorite's pinned URL as received (sync); a tab
  /// that keeps none (not in Pinned or Favorites) ignores it.
  public func rawSetSavedURL(_ node: SidebarNode, url: String, title: String) {
    guard node.isTab, !node.savedURL.isEmpty, !url.isEmpty else { return }
    node.savedURL = url
    node.savedTitle = title
  }

  public func rawDetach(_ node: SidebarNode) { node.parent?.remove(node) }

  public func rawNormalize(_ split: SidebarNode) {
    let saved = remoteDepth
    remoteDepth = 1
    normalizeSplit(split)
    remoteDepth = saved
  }

  public func makeNode(
    id: String, kind: SidebarNode.Kind, title: String = "", customTitle: String = "", url: String = "",
    lastActive: Date? = nil, archivedAt: Date? = nil, collapsed: Bool = false, position: UniquePosition,
    savedURL: String = "", savedTitle: String = ""
  ) -> SidebarNode {
    let node = SidebarNode(
      id: id, kind: kind, title: title, customTitle: customTitle, url: url, lastActive: lastActive,
      archivedAt: archivedAt, collapsed: collapsed, position: position)
    node.savedURL = savedURL
    node.savedTitle = savedTitle
    return node
  }

  public func makeSpace(
    id: String, name: String, theme: SpaceTheme, icon: String, profileUUID: String,
    archiveAfter: TimeInterval?, position: UniquePosition
  ) -> Space {
    Space(
      id: id, name: name, theme: theme, icon: icon, profileUUID: profileUUID, archiveAfter: archiveAfter,
      position: position)
  }

  /// Sets a node's synced fields as received.
  public func rawUpdate(
    _ node: SidebarNode, title: String, customTitle: String, url: String, lastActive: Date?, archivedAt: Date?,
    collapsed: Bool, position: UniquePosition
  ) {
    node.title = title
    node.customTitle = customTitle
    node.url = url
    node.lastActive = lastActive
    node.archivedAt = archivedAt
    node.collapsed = collapsed
    if node.position != position {
      node.position = position
      node.parent?.resort()
    }
  }

  public func rawUpdate(
    _ space: Space, name: String, theme: SpaceTheme, icon: String, profileUUID: String,
    archiveAfter: TimeInterval?, position: UniquePosition
  ) {
    space.name = name
    space.theme = theme
    space.icon = icon
    space.profileUUID = profileUUID
    space.archiveAfter = archiveAfter
    if space.position != position {
      space.position = position
      rawSortSpaces()
    }
  }
}
