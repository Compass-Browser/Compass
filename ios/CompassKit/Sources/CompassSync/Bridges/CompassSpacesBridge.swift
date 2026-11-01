// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import SwiftProtobuf
import SyncProto

/// COMPASS_SPACES on the phone, wire-compatible with the desktop's
/// CompassSpacesSyncBridge (browser/sync/compass_spaces_sync_bridge.cc on
/// sync-main; docs/design/ios.md, "M3 notes", has the rules it follows):
///
/// - One entity per space and per node, client tag = lowercase UUID.
/// - A *mirror* keeps the last specifics the server had for each entity;
///   local commits start from it and overwrite only the fields we own, so
///   fields this client doesn't know survive the round trip.
/// - Position suffixes come from the entity's client tag hash.
/// - Tab content churn (URL, title, last active) is throttled to one commit
///   per entity every 10 s.
/// - Remote children whose parent hasn't arrived wait ("parked") at the end
///   of their section root, and nothing is committed for them meanwhile.
///
/// One per profile account (slice 5; docs/design/ios.md, "M8"): it syncs
/// the spaces of its profile, their entries and the profile's favorites,
/// which its `SpaceDirectory` says it owns. The home profile's also carries
/// the chain's profile list (CompassProfile entities), which the phone only
/// reads.
public final class CompassSpacesBridge: DataTypeBridge, @unchecked Sendable {
  public nonisolated let dataType = DataType.compassSpaces
  public static let throttleInterval: TimeInterval = 10
  static let lastActiveGranularity: Int64 = 60 * 1_000_000  // µs
  static let archiveCap: TimeInterval = 30 * 24 * 60 * 60

  @MainActor let store: SpaceStore
  @MainActor public let directory: SpaceDirectory
  /// The profile whose account this is; empty for the home profile's (the
  /// chain's own account).
  public nonisolated let accountProfile: String
  public nonisolated var isHome: Bool { accountProfile.isEmpty }
  let engine: SyncEngine
  let mirrorURL: URL?
  @MainActor var mirror: [String: SyncPb_CompassSpacesSpecifics] = [:]
  @MainActor var lastCommit: [String: Date] = [:]
  @MainActor var parked: Set<String> = []
  /// Off until the initial merge: before it, local data reaches the chain
  /// only through the merge (the desktop's IsTrackingMetadata()).
  @MainActor public private(set) var tracking = false
  @MainActor var throttled: [String: Task<Void, Never>] = [:]
  @MainActor private var queue: [Op] = []
  @MainActor private var draining = false
  /// Whether an entry (tab or split) is on screen now.
  @MainActor public var isShown: ((String) -> Bool)?
  /// A kept entry got a new ID (old, new): live tabs and selection follow.
  @MainActor public var onReplaced: ((String, String) -> Void)?
  @MainActor var keptEntries: Set<String> = []
  /// Called after local changes were queued (the app schedules a sync).
  @MainActor public var onLocalCommit: (() -> Void)?
  /// A synced site icon arrived: (host, PNG). See +Favicons.
  @MainActor public var onFavicon: ((String, Data) -> Void)?
  @MainActor public var now: () -> Date = Date.init

  struct SavedMirror: Codable {
    var tracking: Bool
    var entities: [String: Data]
  }

  enum Op {
    case put(String, SyncPb_CompassSpacesSpecifics)
    case delete(String)
  }

  /// The only bridge of a store: the home account's, with a directory of
  /// its own.
  @MainActor
  public convenience init(store: SpaceStore, engine: SyncEngine, mirrorURL: URL?) {
    self.init(directory: SpaceDirectory(store: store), profile: "", engine: engine, mirrorURL: mirrorURL)
  }

  /// `profile`: the profile UUID of the account, empty for the home's.
  @MainActor
  public init(directory: SpaceDirectory, profile: String, engine: SyncEngine, mirrorURL: URL?) {
    self.directory = directory
    store = directory.store
    accountProfile = profile
    self.engine = engine
    self.mirrorURL = mirrorURL
    if let mirrorURL, let data = try? Data(contentsOf: mirrorURL),
      let saved = try? JSONDecoder().decode(SavedMirror.self, from: data)
    {
      mirror = saved.entities.compactMapValues { try? SyncPb_CompassSpacesSpecifics(serializedBytes: $0) }
      tracking = saved.tracking
    }
    directory.add(self)
  }

  // MARK: Profiles

  /// The profile UUID this bridge puts on its spaces (the desktop's
  /// GetProfileUuid()): its account's, or the chain's home UUID once known.
  @MainActor var ownProfile: String { isHome ? store.homeProfileUUID : accountProfile }

  @MainActor func owns(_ space: Space) -> Bool { directory.owner(of: space) === self }
  @MainActor func owns(_ node: SidebarNode) -> Bool { directory.owner(of: node) === self }
  @MainActor func ownsSpace(id: String) -> Bool { store.space(id: id).map(owns) ?? false }
  @MainActor var ownFavorites: SidebarNode { store.favorites(forProfile: ownProfile) }

  /// The CompassProfile entities this account has (the home's: the chain's
  /// profile list).
  @MainActor
  public func mirrorProfiles() -> [ChainProfile] {
    mirror.values.compactMap { s -> ChainProfile? in
      guard case .profile(let p) = s.entity, Self.isValidUUID(p.uuid) else { return nil }
      return ChainProfile(p)
    }
  }

  /// The home UUID became known: the home's spaces carry it (as the
  /// desktop's do), and commit it.
  @MainActor
  func homeUUIDChanged() {
    guard isHome else { return }
    let uuid = store.homeProfileUUID
    store.applyingRemote {
      for space in store.spaces where owns(space) && space.profileUUID != uuid {
        store.rawUpdate(
          space, name: space.name, color: space.color, icon: space.icon, profileUUID: uuid,
          archiveAfter: space.archiveAfter, position: space.position)
      }
    }
    guard tracking else { return }
    for space in store.spaces where owns(space) { commit(space.id, desired(space)) }
    drain()
  }

  /// Gives up what another bridge owns now (a space or entry moved to
  /// another profile, here or on another device): its account deletes it,
  /// as the desktop's old profile does after MoveSpaceToProfile(). A node
  /// that still waits for its space here isn't given up.
  @MainActor
  func release(owners: [String: ObjectIdentifier]) {
    let me = ObjectIdentifier(self)
    var released = false
    let before = mirror
    for (id, s) in before {
      guard let owner = owners[id], owner != me else { continue }
      switch s.entity {
      case .space: break
      case .node(let n):
        if n.section != Section.favorites.rawValue {
          guard case .space = before[n.spaceUuid]?.entity else { continue }
        }
      default: continue
      }
      mirror[id] = nil
      lastCommit[id] = nil
      parked.remove(id)
      throttled.removeValue(forKey: id)?.cancel()
      queue.append(.delete(id))
      released = true
    }
    if released { drain() }
  }

  // MARK: Helpers

  public static func suffix(for uuid: String) -> [UInt8] {
    UniquePosition.suffix(clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: uuid))
  }

  /// Microseconds since 1601-01-01 UTC (base::Time's internal value).
  static let windowsEpochOffset: Int64 = 11_644_473_600 * 1_000_000
  static func windowsMicros(_ date: Date) -> Int64 {
    Int64((date.timeIntervalSince1970 * 1_000_000).rounded(.down)) + windowsEpochOffset
  }
  static func date(windowsMicros: Int64) -> Date {
    Date(timeIntervalSince1970: TimeInterval(windowsMicros - windowsEpochOffset) / 1_000_000)
  }

  static func position(_ proto: SyncPb_UniquePosition) -> UniquePosition {
    if proto.hasCustomCompressedV1 { return UniquePosition(compressed: [UInt8](proto.customCompressedV1)) }
    if proto.hasValue, !proto.value.isEmpty { return UniquePosition(uncompressedValue: [UInt8](proto.value)) }
    return UniquePosition(compressed: [])
  }

  static func proto(_ position: UniquePosition) -> SyncPb_UniquePosition {
    var p = SyncPb_UniquePosition()
    p.customCompressedV1 = Data(position.compressed)
    return p
  }

  static func isValidUUID(_ s: String) -> Bool {
    s.count == 36 && s == s.lowercased() && UUID(uuidString: s) != nil
  }

  static func uuid(of s: SyncPb_CompassSpacesSpecifics) -> String? {
    switch s.entity {
    case .space(let space): space.uuid
    case .node(let node): node.uuid
    case .profile(let profile): profile.uuid
    case .favicon(let favicon): favicon.uuid
    case nil: nil
    }
  }

  /// A Today tab nobody else can open (new tab, blank) stays local.
  static func isRestorable(_ url: String) -> Bool {
    guard let u = URL(string: url), let scheme = u.scheme, !scheme.isEmpty else { return false }
    if url == "about:blank" || url.hasPrefix("chrome://newtab") || url.hasPrefix("chrome://new-tab-page")
      || url.hasPrefix("compass://newtab")
    {
      return false
    }
    return true
  }

  public nonisolated func clientTag(for specifics: SyncPb_EntitySpecifics) -> String {
    Self.uuid(of: specifics.compassSpaces) ?? ""
  }

  // MARK: Local → server

  /// What we'd commit for a space now: the mirror with our fields over it.
  @MainActor
  func desired(_ space: Space) -> SyncPb_CompassSpacesSpecifics {
    var specifics = mirror[space.id] ?? SyncPb_CompassSpacesSpecifics()
    var s = specifics.space
    s.uuid = space.id
    if space.name.isEmpty { s.clearName() } else { s.name = space.name }
    s.color = space.color.rawValue
    s.directoryPosition = Self.proto(space.position)
    if !ownProfile.isEmpty { s.profileUuid = ownProfile }
    s.archiveAfterSeconds = Int64(space.archiveAfter ?? 0)
    specifics.space = s
    return specifics
  }

  /// What we'd commit for a node, or nil if it stays local.
  @MainActor
  func desired(_ node: SidebarNode) -> SyncPb_CompassSpacesSpecifics? {
    if parked.contains(node.id) { return mirror[node.id] }
    guard let section = node.section, let type = node.kind.nodeType else { return nil }
    let existing = mirror[node.id]
    if section == .today, existing == nil, !node.tabs.allSatisfy({ Self.isRestorable($0.url) }) || node.isSplit && node.children.isEmpty {
      return nil
    }
    var specifics = existing ?? SyncPb_CompassSpacesSpecifics()
    var n = specifics.node
    let space = store.space(of: node)
    n.uuid = node.id
    if section == .favorites { n.clearSpaceUuid() } else if let space { n.spaceUuid = space.id }
    let parent = node.parent
    if let parent, !parent.isRoot { n.parentUuid = parent.id } else { n.clearParentUuid() }
    n.section = section.rawValue
    n.type = type.rawValue
    if node.isTab, !node.url.isEmpty { n.url = node.url } else { n.clearURL() }
    if node.isSplit || node.title.isEmpty { n.clearTitle() } else { n.title = node.title }
    if node.isTab, !node.customTitle.isEmpty { n.customTitle = node.customTitle } else { n.clearCustomTitle() }
    if node.isFolder, node.collapsed { n.collapsed = true } else { n.clearCollapsed() }

    // A position from the mirror stays only under the same parent, section
    // and space; elsewhere ours goes.
    let sameSlot =
      existing.map {
        $0.node.section == n.section && $0.node.spaceUuid == n.spaceUuid && $0.node.parentUuid == n.parentUuid
      } ?? false

    if section == .archive {
      let entryRoot = parent?.isRoot == true
      if let at = node.archivedAt ?? parent?.archivedAt { n.archivedTime = Self.windowsMicros(at) }
      if entryRoot {
        n.clearCustomTitle()
        n.clearCollapsed()
        if node.isSplit { n.clearURL(); n.clearTitle() }
        // The archive is ordered by time; keep what it had in Today.
        if !(existing?.node.hasPosition ?? false) { n.position = Self.proto(node.position) }
      } else if !sameSlot || !(existing?.node.hasPosition ?? false) {
        n.position = Self.proto(node.position)
      }
    } else {
      n.clearArchivedTime()
      n.position = Self.proto(node.position)
      if section == .today, node.isTab, let seen = node.lastActive ?? parent?.lastActive {
        let local = Self.windowsMicros(seen)
        if !n.hasLastActiveTime || local - n.lastActiveTime >= Self.lastActiveGranularity {
          n.lastActiveTime = local
        }
      }
    }
    specifics.node = n
    return specifics
  }

  @MainActor
  func localChanged(_ change: SpaceStoreChange) {
    guard tracking else { return }
    for id in change.deletedSpaces.union(change.deletedNodes) where mirror[id] != nil {
      mirror[id] = nil
      lastCommit[id] = nil
      throttled.removeValue(forKey: id)?.cancel()
      queue.append(.delete(id))
    }
    for id in change.putSpaces {
      if let space = store.space(id: id), owns(space) { commit(id, desired(space)) }
    }
    for id in change.putNodes.subtracting(change.deletedNodes) {
      if let node = store.node(id: id), owns(node), let specifics = desired(node) { commit(id, specifics) }
    }
    for id in change.todayChurn.subtracting(change.putNodes) {
      guard let node = store.node(id: id), owns(node) else { continue }
      for n in [node] + node.children { commitThrottled(n) }
    }
    drain()
    if !change.deletedNodes.isEmpty { collectFavicons() }
  }

  @MainActor
  func queueDelete(_ id: String) {
    throttled.removeValue(forKey: id)?.cancel()
    queue.append(.delete(id))
  }

  @MainActor
  func commit(_ id: String, _ specifics: SyncPb_CompassSpacesSpecifics?) {
    guard let specifics, specifics != mirror[id] else { return }
    mirror[id] = specifics
    lastCommit[id] = now()
    throttled.removeValue(forKey: id)?.cancel()
    queue.append(.put(id, specifics))
  }

  /// URL/title/last-active churn of a tab commits at most every 10 s.
  @MainActor
  func commitThrottled(_ node: SidebarNode) {
    guard let specifics = desired(node), specifics != mirror[node.id] else { return }
    if let last = lastCommit[node.id], let old = mirror[node.id],
      Self.contentOnly(old, specifics), now().timeIntervalSince(last) < Self.throttleInterval
    {
      guard throttled[node.id] == nil else { return }
      let delay = Self.throttleInterval - now().timeIntervalSince(last)
      let id = node.id
      throttled[id] = Task { @MainActor [weak self] in
        try? await Task.sleep(for: .seconds(delay))
        guard let self, !Task.isCancelled else { return }
        self.throttled[id] = nil
        if let node = self.store.node(id: id), self.owns(node) { self.commit(id, self.desired(node)) }
        self.drain()
      }
      return
    }
    commit(node.id, specifics)
  }

  /// Whether two node specifics differ only in url, title or last active.
  static func contentOnly(_ a: SyncPb_CompassSpacesSpecifics, _ b: SyncPb_CompassSpacesSpecifics) -> Bool {
    guard case .node(var x) = a.entity, case .node(var y) = b.entity, x.type == NodeType.tab.rawValue else {
      return false
    }
    x.clearURL(); x.clearTitle(); x.clearLastActiveTime()
    y.clearURL(); y.clearTitle(); y.clearLastActiveTime()
    return x == y
  }

  @MainActor
  func drain() {
    saveMirror()
    guard !draining, !queue.isEmpty else { return }
    draining = true
    let ops = queue
    queue.removeAll()
    Task { [weak self] in
      guard let self else { return }
      for op in ops {
        switch op {
        case .put(let id, let specifics):
          var e = SyncPb_EntitySpecifics()
          e.compassSpaces = specifics
          await self.engine.put(.compassSpaces, tag: id, specifics: e)
        case .delete(let id):
          await self.engine.delete(.compassSpaces, tag: id)
        }
      }
      await MainActor.run {
        self.draining = false
        self.onLocalCommit?()
        self.drain()
      }
    }
  }

  @MainActor
  func saveMirror() {
    guard let mirrorURL else { return }
    let saved = SavedMirror(
      tracking: tracking, entities: mirror.compactMapValues { try? $0.serializedBytes() as Data })
    try? JSONEncoder().encode(saved).write(to: mirrorURL, options: .atomic)
  }

  /// Waits until queued changes reached the engine (tests).
  @MainActor
  public func flush() async {
    while draining || !queue.isEmpty {
      if !draining { drain() }
      try? await Task.sleep(for: .milliseconds(5))
    }
  }

  // MARK: Server → local

  public nonisolated func apply(_ changes: [RemoteChange]) async {
    await MainActor.run { self.applyRemote(changes) }
  }

  @MainActor
  func applyRemote(_ changes: [RemoteChange]) {
    var putSpaces: [String] = []
    var putNodes: [String] = []
    var deleted: Set<String> = []
    var profilesChanged = false
    var favicons: [(String, Data)] = []
    let byHash = Dictionary(
      mirror.keys.map { (ClientTagHash.make(type: .compassSpaces, tag: $0), $0) }, uniquingKeysWith: { a, _ in a })
    for change in changes {
      if let specifics = change.specifics?.compassSpaces {
        guard let id = Self.uuid(of: specifics), Self.isValidUUID(id) else { continue }
        mirror[id] = specifics
        lastCommit[id] = nil
        switch specifics.entity {
        case .space: putSpaces.append(id)
        case .node: putNodes.append(id)
        case .profile: profilesChanged = true
        case .favicon(let f): if !f.host.isEmpty && !f.png.isEmpty { favicons.append((f.host, f.png)) }
        case nil: break
        }
      } else if let id = byHash[change.clientTagHash] {
        if case .profile = mirror[id]?.entity { profilesChanged = true }
        mirror[id] = nil
        deleted.insert(id)
      }
    }
    // First: the home UUID may come with the list, and the spaces below
    // carry it (the desktop's OnChainProfiles() before its pass).
    if profilesChanged && isHome { directory.homeProfilesChanged() }

    store.applyingRemote {
      applySpaces(putSpaces, deleted: deleted)
      // Nodes that waited for a space that's ours now (it came, or was
      // adopted from another profile).
      let waiting = mirror.compactMap { id, s -> String? in
        guard case .node(let n) = s.entity, !putNodes.contains(id),
          store.node(id: id).map({ !owns($0) }) ?? true,
          n.section == Section.favorites.rawValue || ownsSpace(id: n.spaceUuid)
        else { return nil }
        return id
      }
      applyNodes(putNodes + waiting)
      applyNodeDeletions(deleted)
      place()
    }
    recreateKeptEntries()
    // Fix-ups (cycles, invalid placements, normalized splits) commit;
    // everything else equals the mirror and doesn't.
    for id in Set(putNodes).union(parked) {
      if let node = store.node(id: id), owns(node), let s = desired(node) { commit(id, s) }
    }
    drain()
    // What came here from another profile leaves that profile's account.
    directory.reconcile()
    for (host, png) in favicons { onFavicon?(host, png) }
    collectFavicons()
  }

  @MainActor
  func applySpaces(_ ids: [String], deleted: Set<String>) {
    for id in ids {
      let s = mirror[id]!.space
      guard store.node(id: id) == nil else { continue }
      let color = (1...8).contains(s.color) ? SpaceColor(rawValue: s.color) : nil
      // Outer nil: not set (keep ours); inner nil: 0 = never.
      var archiveAfter: TimeInterval?? = .none
      if s.hasArchiveAfterSeconds && s.archiveAfterSeconds >= 0 {
        archiveAfter = .some(s.archiveAfterSeconds == 0 ? nil : TimeInterval(s.archiveAfterSeconds))
      }
      if let space = store.space(id: id) {
        // Ours, or another profile's here: then it was moved to this
        // profile on another device, and comes over with its entries
        // (the desktop's AdoptSpace()); `reconcile()` then deletes it from
        // the old profile's account.
        store.rawUpdate(
          space, name: s.name.isEmpty ? space.name : s.name, color: color ?? space.color, icon: s.icon,
          profileUUID: ownProfile, archiveAfter: archiveAfter ?? space.archiveAfter,
          position: s.hasDirectoryPosition ? Self.position(s.directoryPosition) : space.position)
      } else {
        let space = store.makeSpace(
          id: id, name: s.name.isEmpty ? "Space" : s.name, color: color ?? .blue, icon: s.icon,
          profileUUID: ownProfile, archiveAfter: archiveAfter ?? 12 * 60 * 60,
          position: s.hasDirectoryPosition
            ? Self.position(s.directoryPosition)
            : UniquePosition.forInsert(after: store.spaces.last?.position, before: nil, suffix: Self.suffix(for: id)))
        store.rawInsertSpace(space)
      }
    }
    for id in deleted {
      // Only ours: one another profile has now was moved there (its
      // account brought it first). The last space stays.
      if let space = store.space(id: id), owns(space), store.spaces.count > 1 { store.rawRemoveSpace(space) }
    }
    store.rawSortSpaces()
  }

  /// Creates or updates nodes in place; `place()` moves them where they go.
  @MainActor
  func applyNodes(_ ids: [String]) {
    for id in ids {
      guard let n = mirror[id]?.node, let section = Section(rawValue: n.section),
        let type = NodeType(rawValue: n.type)
      else { continue }
      if section == .favorites && type != .tab { continue }
      // Waits until its space is here and ours.
      if section != .favorites && !ownsSpace(id: n.spaceUuid) { continue }
      let kind: SidebarNode.Kind =
        switch type {
        case .tab: .tab
        case .folder: .folder
        case .split: .split
        }
      let lastActive = n.hasLastActiveTime ? Self.date(windowsMicros: n.lastActiveTime) : nil
      let archivedAt = n.hasArchivedTime ? Self.date(windowsMicros: n.archivedTime) : nil
      if let node = store.node(id: id) {
        guard node.kind == kind else { continue }  // a node's type never changes
        let keepContent = throttled[id] != nil
        store.rawUpdate(
          node, title: keepContent ? node.title : n.title, customTitle: n.customTitle,
          url: keepContent ? node.url : n.url,
          lastActive: max(node.lastActive ?? .distantPast, lastActive ?? .distantPast) == .distantPast
            ? nil : max(node.lastActive ?? .distantPast, lastActive ?? .distantPast),
          archivedAt: archivedAt, collapsed: n.collapsed, position: node.position)
      } else {
        let node = store.makeNode(
          id: id, kind: kind, title: n.title, customTitle: n.customTitle, url: n.url, lastActive: lastActive,
          archivedAt: archivedAt ?? (section == .archive ? now() : nil), collapsed: n.collapsed,
          position: Self.position(n.position))
        guard let root = root(for: n) else { continue }
        store.rawInsert(node, into: root)
      }
    }
  }

  @MainActor
  func root(for n: SyncPb_CompassNode) -> SidebarNode? {
    guard let section = Section(rawValue: n.section) else { return nil }
    if section == .favorites { return ownFavorites }
    guard let space = store.space(id: n.spaceUuid), owns(space) else { return nil }
    return space.root(section)
  }

  @MainActor
  func applyNodeDeletions(_ ids: Set<String>) {
    for id in ids {
      // Only ours: another profile's has moved there.
      guard let node = store.node(id: id), owns(node), let root = Optional(node.root) else { continue }
      // The Today entry on screen survives a close elsewhere: it's
      // re-created under fresh UUIDs once this batch is applied.
      if node.section == .today, let entry = Self.entry(of: node), isShown?(entry.id) == true {
        keptEntries.insert(entry.id)
        continue
      }
      // Children that weren't deleted themselves go to the end of the root.
      for child in node.children where !ids.contains(child.id) {
        store.rawDetach(child)
        store.rawInsert(child, into: root)
        parked.insert(child.id)
      }
      let parent = node.parent
      store.rawDetach(node)
      if let parent, parent.isSplit { store.rawNormalize(parent) }
    }
  }

  /// A tab, or the split it's a pane of.
  static func entry(of node: SidebarNode) -> SidebarNode? {
    node.parent?.isSplit == true ? node.parent : (node.isTab || node.isSplit ? node : nil)
  }

  /// The desktop keeps a Today entry shown in the focused window when
  /// another device closes it, under fresh UUIDs so no device re-creates a
  /// deleted one (bridge.cc, ApplyNodeDeletions). Same here for the entry
  /// on screen.
  @MainActor
  func recreateKeptEntries() {
    let kept = keptEntries
    keptEntries.removeAll()
    for id in kept {
      guard let entry = store.node(id: id), let parent = entry.parent else { continue }
      let index = parent.children.firstIndex { $0 === entry } ?? parent.children.count
      let tabs = entry.tabs
      var replacement: SidebarNode?
      store.batch {
        guard let first = tabs.first,
          let copy = store.addTab(url: first.url, title: first.title, to: parent, at: index, now: now())
        else { return }
        replacement = copy
        onReplaced?(first.id, copy.id)
        if tabs.count > 1,
          let second = store.addTab(url: tabs[1].url, title: tabs[1].title, to: parent, at: index + 1, now: now())
        {
          onReplaced?(tabs[1].id, second.id)
          replacement = store.split(copy, with: second)
        }
      }
      store.applyingRemote {
        for tab in tabs { mirror[tab.id] = nil }
        mirror[id] = nil
        store.rawDetach(entry)
      }
      if let replacement { onReplaced?(id, replacement.id) }
    }
  }

  /// Puts every mirrored node under its parent, parents first. Unknown
  /// parents park the node; invalid placements send it to the section root.
  @MainActor
  func place() {
    parked.removeAll()
    var touchedSplits: [SidebarNode] = []
    let nodes = mirror.compactMap { id, s -> (SidebarNode, SyncPb_CompassNode)? in
      guard case .node(let n) = s.entity, let node = store.node(id: id) else { return nil }
      return (node, n)
    }
    func depth(_ n: SyncPb_CompassNode) -> Int {
      var d = 0
      var parent = n.parentUuid
      while !parent.isEmpty, d < 64, let p = mirror[parent]?.node {
        d += 1
        parent = p.parentUuid
      }
      return d
    }
    for (node, n) in nodes.sorted(by: { depth($0.1) < depth($1.1) }) {
      guard let root = root(for: n) else { continue }
      var target = root
      if !n.parentUuid.isEmpty {
        if let parent = store.node(id: n.parentUuid) {
          if parent.root === root, store.canMove(node, to: parent) || node.parent === parent {
            target = parent
          }
        } else {
          parked.insert(node.id)
        }
      }
      let position = Self.position(n.position)
      if node.parent !== target || (position.isValid && node.position != position) {
        let old = node.parent
        store.rawDetach(node)
        store.rawUpdate(
          node, title: node.title, customTitle: node.customTitle, url: node.url, lastActive: node.lastActive,
          archivedAt: node.archivedAt, collapsed: node.collapsed,
          position: position.isValid
            ? position
            : UniquePosition.forInsert(after: target.children.last?.position, before: nil, suffix: Self.suffix(for: node.id)))
        store.rawInsert(node, into: target)
        if let old, old.isSplit { touchedSplits.append(old) }
      }
      if target.isSplit, target.children.count > SpaceStore.maxSplitPanes {
        store.rawDetach(node)
        store.rawInsert(node, into: root)
      }
    }
    for split in touchedSplits { store.rawNormalize(split) }
  }

  // MARK: Initial merge

  public nonisolated func localDataForInitialMerge() async -> [(tag: String, specifics: SyncPb_EntitySpecifics)] {
    await MainActor.run { self.mergeWithChain() }
  }

  /// The desktop's MergeWithChain + DedupeToday, then every local entity the
  /// chain doesn't have.
  @MainActor
  func mergeWithChain() -> [(tag: String, specifics: SyncPb_EntitySpecifics)] {
    store.applyingRemote {
      // Favorites: drop local ones the chain has by URL.
      let chainFavoriteURLs = Set(
        mirror.values.compactMap { s -> String? in
          guard case .node(let n) = s.entity, n.section == Section.favorites.rawValue else { return nil }
          return n.url
        })
      for fav in ownFavorites.children where mirror[fav.id] == nil && chainFavoriteURLs.contains(fav.url) {
        store.rawDetach(fav)
      }
      // An untouched local "Home" (empty Pinned) folds its Today into the
      // chain's first space.
      if let first = store.spaces.first(where: { mirror[$0.id] != nil && owns($0) }) {
        for space in store.spaces
        where mirror[space.id] == nil && owns(space) && space.name == "Home" && space.pinned.children.isEmpty {
          for entry in space.today.children {
            store.rawDetach(entry)
            store.rawUpdate(
              entry, title: entry.title, customTitle: entry.customTitle, url: entry.url,
              lastActive: entry.lastActive, archivedAt: entry.archivedAt, collapsed: entry.collapsed,
              position: UniquePosition.forInsert(
                after: first.today.children.last?.position, before: nil, suffix: Self.suffix(for: entry.id)))
            store.rawInsert(entry, into: first.today)
          }
          store.rawRemoveSpace(space)
        }
      }
      // Today: an own tab with the same URL as a chain tab goes.
      for space in store.spaces where owns(space) {
        let tabs = space.today.children.flatMap(\.subtree).filter { $0.isTab && $0.parent?.isSplit != true }
        let chainURLs = Set(tabs.filter { mirror[$0.id] != nil }.map(\.url))
        for tab in tabs where mirror[tab.id] == nil && chainURLs.contains(tab.url) { store.rawDetach(tab) }
      }
      // Local spaces go after the chain's.
      var last = store.spaces.filter { mirror[$0.id] != nil && owns($0) }.map(\.position).max()
      for space in store.spaces where mirror[space.id] == nil && owns(space) {
        let position = UniquePosition.forInsert(after: last, before: nil, suffix: Self.suffix(for: space.id))
        store.rawUpdate(
          space, name: space.name, color: space.color, icon: space.icon,
          profileUUID: isHome ? space.profileUUID : ownProfile, archiveAfter: space.archiveAfter, position: position)
        last = position
      }
    }
    var result: [(tag: String, specifics: SyncPb_EntitySpecifics)] = []
    func add(_ id: String, _ s: SyncPb_CompassSpacesSpecifics?) {
      guard let s, mirror[id] == nil else { return }
      mirror[id] = s
      var e = SyncPb_EntitySpecifics()
      e.compassSpaces = s
      result.append((id, e))
    }
    for space in store.spaces where owns(space) { add(space.id, desired(space)) }
    for node in store.allNodes where owns(node) { add(node.id, desired(node)) }
    tracking = true
    saveMirror()
    return result
  }
}
