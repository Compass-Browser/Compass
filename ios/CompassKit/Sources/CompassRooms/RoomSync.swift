// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassEasels
import CompassModel
import CompassSync
import Foundation
import SwiftProtobuf
import SyncProto

/// One room on this device: its records, decrypted, mirrored into the
/// sidebar (or the Easels) within the shared item, and local edits there
/// written back, encrypted. The mirror-diff pattern of CompassSpacesBridge,
/// scoped to the mounted subtree (docs/design/ios.md, "Rooms"):
///
/// - `mirror` keeps each record as the server last had it (with its `seq`),
///   so local commits start from it and fields this client doesn't know
///   survive;
/// - a cycle writes what differs from the mirror (with `base_seq`: a record
///   someone changed meanwhile is a conflict, and theirs wins), then reads
///   what's new since the last `seq` and applies it;
/// - a viewer writes nothing: its local edits go back to the room's;
/// - the shared item itself is never deleted by a diff: one gone here comes
///   back (Leave or Delete for Everyone end a room).
@MainActor
public final class RoomSync {
  public let roomID: String
  public let roomIDBytes: Data
  public let kind: RoomKind
  public internal(set) var mount: RoomMount
  /// The room as last read (members, epoch); nil before.
  public internal(set) var info: RoomInfo?
  let store: SpaceStore
  let easels: EaselStore?
  let stateURL: URL?
  /// The profile UUID a shared space made here gets (its account's).
  var spaceProfile: () -> String = { "" }
  /// Where a shared folder or easel tab goes when its placement isn't here.
  var fallbackSpace: () -> Space? = { nil }

  struct Entry: Codable, Equatable {
    var seq: Int64
    /// The serialized specifics; nil for a deleted record.
    var data: Data?
  }

  struct State: Codable {
    var seq: Int64 = 0
    var entries: [String: Entry] = [:]
    /// Owner: the last epoch whose keys each member was sent.
    var envelopesSent: [String: UInt32] = [:]
    /// Records that couldn't be read yet (no key for their epoch).
    var unreadable: Set<String> = []
  }

  var state = State()
  var mirror: [String: SyncPb_CompassSpacesSpecifics] = [:]
  /// Boards shared with this room (the easel, or those its tabs show).
  public private(set) var easelIDs: Set<String> = []

  init(mount: RoomMount, store: SpaceStore, easels: EaselStore?, stateURL: URL?) {
    self.mount = mount
    roomID = mount.roomID
    roomIDBytes = mount.roomIDBytes ?? Data(count: RoomCrypto.roomIDLength)
    kind = mount.roomKind ?? .folder
    self.store = store
    self.easels = easels
    self.stateURL = stateURL
    if let stateURL, let data = try? Data(contentsOf: stateURL),
      let saved = try? JSONDecoder().decode(State.self, from: data)
    {
      state = saved
      for (id, entry) in saved.entries {
        if let bytes = entry.data, let s = try? SyncPb_CompassSpacesSpecifics(serializedBytes: bytes) { mirror[id] = s }
      }
    }
    refreshEaselIDs()
  }

  public var role: RoomRole { info?.role ?? mount.roomRole }
  public var rootUUID: String { mount.rootUuid }
  public var keys: [UInt32: Data] { mount.keyByEpoch }
  public var epoch: UInt32 { info?.epoch ?? keys.keys.max() ?? 1 }
  public var name: String {
    switch kind {
    case .space: if case .space(let s)? = mirror[rootUUID]?.entity, !s.name.isEmpty { return s.name }
    case .folder: if let node = store.node(id: rootUUID), !node.title.isEmpty { return node.title }
    case .easel: if let board = easels?.easel(rootUUID), !board.title.isEmpty { return board.title }
    }
    return mount.name
  }

  func save() {
    guard let stateURL else { return }
    var entries: [String: Entry] = [:]
    for (id, entry) in state.entries { entries[id] = entry }
    state.entries = entries
    try? JSONEncoder().encode(state).write(to: stateURL, options: .atomic)
  }

  func remember(_ id: String, seq: Int64, _ specifics: SyncPb_CompassSpacesSpecifics?) {
    let kept = specifics.map(Self.mirrored)
    mirror[id] = kept
    state.entries[id] = Entry(seq: seq, data: kept.flatMap { try? $0.serializedBytes() })
  }

  /// An image without its bytes (the store has them).
  static func mirrored(_ s: SyncPb_CompassSpacesSpecifics) -> SyncPb_CompassSpacesSpecifics {
    guard case .easelAsset(var asset) = s.entity, asset.hasData else { return s }
    asset.clearData()
    var out = s
    out.easelAsset = asset
    return out
  }

  // MARK: Scope

  /// The shared folder, here.
  var rootNode: SidebarNode? { kind == .folder ? store.node(id: rootUUID) : nil }
  var space: Space? { kind == .space ? store.space(id: rootUUID) : nil }
  public var easelTabID: String { RoomRecords.easelTabID(roomID: roomID) }

  /// Whether a node is the room's.
  public func contains(_ node: SidebarNode) -> Bool {
    guard !rootUUID.isEmpty else { return false }
    switch kind {
    case .folder:
      var current: SidebarNode? = node
      while let c = current {
        if c.id == rootUUID { return true }
        current = c.parent
      }
      return false
    case .space:
      return node.section == .pinned && store.space(of: node)?.id == rootUUID
    case .easel:
      return node.id == easelTabID
    }
  }

  /// Whether a node ID (here or not yet) is the room's.
  public func containsNode(id: String) -> Bool {
    if id == rootUUID && kind == .folder { return true }
    if kind == .easel && id == easelTabID { return true }
    if case .node? = mirror[id]?.entity { return true }
    if let node = store.node(id: id) { return contains(node) }
    return false
  }

  /// The nodes of the room here, the root first.
  var scopeNodes: [SidebarNode] {
    switch kind {
    case .folder: rootNode.map(\.subtree) ?? []
    case .space: space.map { $0.pinned.children.flatMap(\.subtree) } ?? []
    case .easel: []
    }
  }

  func refreshEaselIDs() {
    var ids: Set<String> = []
    switch kind {
    case .easel: if !rootUUID.isEmpty { ids.insert(rootUUID) }
    case .folder, .space:
      for node in scopeNodes where node.isTab {
        if let board = EaselURL.uuid(from: node.url) { ids.insert(board) }
      }
    }
    for s in mirror.values { if case .easel(let e) = s.entity { ids.insert(e.uuid) } }
    easelIDs = ids
  }

  /// Image UUIDs the room's boards show.
  var assetIDs: Set<String> {
    var out: Set<String> = []
    for s in mirror.values { if case .easelAsset(let a) = s.entity { out.insert(a.uuid) } }
    guard let easels else { return out }
    for board in easelIDs {
      for object in easels.objects(of: board) {
        for sha in Easels.assets(of: object) { out.insert(Easels.assetUUID(sha256: sha)) }
      }
    }
    return out
  }

  // MARK: Local → room

  /// What the room should hold now, by record ID.
  func desired() -> [String: SyncPb_CompassSpacesSpecifics] {
    refreshEaselIDs()
    var out: [String: SyncPb_CompassSpacesSpecifics] = [:]
    switch kind {
    case .folder:
      guard let root = rootNode else { break }
      for node in root.subtree {
        guard var s = CompassSpacesBridge.nodeSpecifics(node, existing: mirror[node.id], store: store) else { continue }
        var n = s.node
        n.clearSpaceUuid()
        n.section = Section.pinned.rawValue
        if node === root {
          n.clearParentUuid()
          n.clearPosition()
        }
        s.node = n
        out[node.id] = s
      }
    case .space:
      guard let space else { break }
      out[space.id] = CompassSpacesBridge.spaceSpecifics(space, existing: mirror[space.id])
      for node in space.pinned.children.flatMap(\.subtree) {
        if let s = CompassSpacesBridge.nodeSpecifics(node, existing: mirror[node.id], store: store) { out[node.id] = s }
      }
    case .easel:
      break
    }
    if let easels {
      for board in easelIDs {
        guard let e = easels.easel(board) else { continue }
        var s = SyncPb_CompassSpacesSpecifics()
        s.easel = e
        out[e.uuid] = s
        for object in easels.objects(of: board) {
          var o = SyncPb_CompassSpacesSpecifics()
          o.easelObject = object
          out[object.uuid] = o
          for sha in Easels.assets(of: object) {
            guard let asset = easels.asset(sha) else { continue }
            if case .easelAsset? = mirror[asset.uuid]?.entity {
              out[asset.uuid] = mirror[asset.uuid]
              continue
            }
            guard let data = easels.assetData(sha) else { continue }
            var full = asset
            full.data = data
            var a = SyncPb_CompassSpacesSpecifics()
            a.easelAsset = full
            out[asset.uuid] = a
          }
        }
      }
    }
    return out
  }

  /// Whether the shared item itself is here (a room whose item is gone
  /// here writes no deletions: it's put back instead).
  var rootIsHere: Bool {
    switch kind {
    case .folder: rootNode != nil
    case .space: space != nil
    case .easel: easels?.easel(rootUUID) != nil
    }
  }

  /// The writes that bring the room to what's here: (id, specifics or nil
  /// to delete, base seq).
  func pendingWrites() -> [(id: String, specifics: SyncPb_CompassSpacesSpecifics?, base: Int64)] {
    guard !rootUUID.isEmpty, rootIsHere else { return [] }
    let want = desired()
    var out: [(String, SyncPb_CompassSpacesSpecifics?, Int64)] = []
    for id in want.keys.sorted() {
      let s = want[id]!
      if Self.mirrored(s) == mirror[id] { continue }
      out.append((id, s, state.entries[id]?.seq ?? 0))
    }
    for (id, s) in mirror where want[id] == nil {
      // The item itself stays; an image may still be shown by a board
      // that's coming.
      if id == rootUUID { continue }
      if case .easelAsset = s.entity { continue }
      out.append((id, nil, state.entries[id]?.seq ?? 0))
    }
    return out
  }

  /// Writes local edits (editors and the owner). A conflict applies the
  /// stored records (theirs win); a stale epoch reads the room's keys again
  /// and retries once.
  func push(_ client: RoomClient, identity: IdentityKey, retried: Bool = false) async throws {
    guard role.canEdit, !(info?.pending ?? false) else {
      revertLocalEdits()
      return
    }
    let writes = pendingWrites()
    guard !writes.isEmpty else { return }
    let epoch = self.epoch
    guard let key = keys[epoch] else {
      _ = try await refresh(client, identity: identity)
      if !retried { try await push(client, identity: identity, retried: true) }
      return
    }
    // At most 500 records and about 12 MiB a write.
    var batches: [[RoomWrite]] = [[]]
    var bytes = 0
    var sent: [String: SyncPb_CompassSpacesSpecifics?] = [:]
    for w in writes {
      var write = RoomWrite(id: w.id, epoch: epoch, ciphertext: nil, deleted: w.specifics == nil, baseSeq: w.base)
      if let s = w.specifics {
        let plain: Data = try s.serializedBytes()
        write.ciphertext = try RoomCrypto.encryptRecord(
          roomKey: key, roomID: roomIDBytes, recordID: w.id, epoch: epoch, plaintext: plain)
      }
      let size = write.ciphertext?.count ?? 0
      if batches[batches.count - 1].count >= 500 || (bytes + size > 12 * 1024 * 1024 && !batches[batches.count - 1].isEmpty) {
        batches.append([])
        bytes = 0
      }
      batches[batches.count - 1].append(write)
      bytes += size
      sent[w.id] = w.specifics
    }
    for batch in batches where !batch.isEmpty {
      do {
        let result = try await client.write(roomID, records: batch)
        for written in result.records { remember(written.id, seq: written.seq, sent[written.id] ?? nil) }
        save()
      } catch RoomError.conflict(let current) {
        let (changed, deleted) = ingest(current)
        apply(changed: changed, deleted: deleted)
        save()
        return
      } catch RoomError.staleEpoch {
        _ = try await refresh(client, identity: identity)
        if !retried { try await push(client, identity: identity, retried: true) }
        return
      }
    }
  }

  // MARK: Room → local

  /// Reads what's new (waiting up to `wait` seconds when there's nothing)
  /// and applies it. Whether anything came.
  @discardableResult
  func pull(_ client: RoomClient, identity: IdentityKey, wait: Int = 0) async throws -> Bool {
    var any = false
    var waitNow = wait
    while true {
      let page = try await client.changes(roomID, since: state.seq, wait: waitNow)
      waitNow = 0
      if page.records.contains(where: { keys[$0.epoch] == nil }) || page.epoch > epoch {
        _ = try await refresh(client, identity: identity)
      }
      let (changed, deleted) = ingest(page.records)
      if !changed.isEmpty || !deleted.isEmpty {
        apply(changed: changed, deleted: deleted)
        any = true
      }
      state.seq = page.more ? (page.records.last?.seq ?? page.seq) : max(state.seq, page.seq)
      save()
      if !page.more { break }
    }
    if mount.rootUuid.isEmpty, let root = RoomRecords.rootUUID(kind: kind, records: mirror) {
      mount.rootUuid = root
      apply(changed: mirror, deleted: [])
      onMountChanged?(mount)
    }
    return any
  }

  /// Called when the mount changes here (keys, role, root, placement): the
  /// account commits it.
  var onMountChanged: ((RoomMount) -> Void)?

  /// Takes records into the mirror: what changed and what was deleted. A
  /// record that can't be read (no key yet, tampered) or isn't what a room
  /// may hold is skipped.
  func ingest(_ records: [RoomRecord]) -> (changed: [String: SyncPb_CompassSpacesSpecifics], deleted: Set<String>) {
    var changed: [String: SyncPb_CompassSpacesSpecifics] = [:]
    var deleted: Set<String> = []
    let keys = self.keys
    for r in records {
      if r.deleted || r.ciphertext == nil {
        if mirror[r.id] != nil || state.entries[r.id] == nil || r.seq > 0 {
          if mirror[r.id] != nil { deleted.insert(r.id) }
          remember(r.id, seq: r.seq, nil)
          changed[r.id] = nil
        }
        state.unreadable.remove(r.id)
        continue
      }
      guard let key = keys[r.epoch], let sealed = r.ciphertext,
        let plain = RoomCrypto.decryptRecord(roomKey: key, roomID: roomIDBytes, recordID: r.id, epoch: r.epoch, sealed: sealed)
      else {
        state.unreadable.insert(r.id)
        continue
      }
      state.unreadable.remove(r.id)
      guard let s = try? SyncPb_CompassSpacesSpecifics(serializedBytes: plain), isAllowed(id: r.id, s) else { continue }
      remember(r.id, seq: r.seq, s)
      changed[r.id] = s
      deleted.remove(r.id)
    }
    return (changed, deleted)
  }

  /// A record's entity must be under its own ID, of a kind rooms carry,
  /// within the room, and not take over a node of this device that isn't
  /// the room's.
  func isAllowed(id: String, _ s: SyncPb_CompassSpacesSpecifics) -> Bool {
    guard RoomRecords.recordID(of: s) == id, CompassSpacesBridge.isValidUUID(id) else { return false }
    switch s.entity {
    case .space:
      guard kind == .space, rootUUID.isEmpty || id == rootUUID else { return false }
      if let existing = store.space(id: id), !rootUUID.isEmpty, existing.id != rootUUID { return false }
      return true
    case .node(let n):
      guard kind != .easel, n.section == Section.pinned.rawValue else { return false }
      if kind == .space, !rootUUID.isEmpty, n.spaceUuid != rootUUID { return false }
      if let node = store.node(id: id), !contains(node), !(kind == .folder && id == rootUUID) { return false }
      return true
    case .easel, .easelObject, .easelAsset:
      return true
    default:
      return false
    }
  }

  /// Applies records to the sidebar and the Easels, within the room.
  func apply(changed: [String: SyncPb_CompassSpacesSpecifics], deleted: Set<String>, force: Bool = false) {
    var board = EaselEntities()
    var boardDeleted: Set<String> = []
    for (_, s) in changed {
      switch s.entity {
      case .easel(let e): board.easels.append(e)
      case .easelObject(let o): board.objects.append(o)
      case .easelAsset(let a): board.assets.append(a)
      default: break
      }
    }
    for id in deleted where mirror[id] == nil {
      if store.node(id: id) == nil, store.space(id: id) == nil { boardDeleted.insert(id) }
    }
    // Board images come with their bytes only once: the mirror has none.
    if let easels {
      board.assets.removeAll { easels.asset($0.sha256) != nil }
      board.assets = board.assets.filter { $0.hasData }
      easels.applyRemote(board, deleted: boardDeleted.subtracting([rootUUID]), force: force || !role.canEdit)
    }
    guard !rootUUID.isEmpty else { return }
    store.applyingRemote {
      if kind == .space, case .space(let s)? = mirror[rootUUID]?.entity { applySpace(s) }
      let nodeIDs = changed.keys.filter { if case .node? = changed[$0]?.entity { true } else { false } }
      applyNodes(nodeIDs)
      for id in deleted where id != rootUUID {
        guard let node = store.node(id: id), contains(node), node.parent != nil else { continue }
        let scopeRoot = self.scopeRoot
        for child in node.children where !deleted.contains(child.id) {
          store.rawDetach(child)
          if let scopeRoot { store.rawInsert(child, into: scopeRoot) }
        }
        let parent = node.parent
        store.rawDetach(node)
        if let parent, parent.isSplit { store.rawNormalize(parent) }
      }
      place()
      if kind == .easel { placeEaselTab() }
    }
    refreshEaselIDs()
  }

  func applySpace(_ s: SyncPb_CompassSpace) {
    if let space = store.space(id: rootUUID) {
      store.rawUpdate(
        space, name: s.name.isEmpty ? space.name : s.name,
        theme: CompassSpacesBridge.theme(of: s, over: space.theme), icon: s.hasIcon ? s.icon : space.icon,
        profileUUID: space.profileUUID, archiveAfter: space.archiveAfter, position: space.position)
    } else {
      let position =
        mount.hasPosition && CompassSpacesBridge.position(mount.position).isValid
        ? CompassSpacesBridge.position(mount.position)
        : UniquePosition.forInsert(
          after: store.spaces.last?.position, before: nil, suffix: CompassSpacesBridge.suffix(for: rootUUID))
      let space = store.makeSpace(
        id: rootUUID, name: s.name.isEmpty ? "Shared Space" : s.name,
        theme: CompassSpacesBridge.theme(of: s, over: .blue), icon: s.icon, profileUUID: spaceProfile(),
        archiveAfter: 12 * 60 * 60, position: position)
      store.rawInsertSpace(space)
    }
  }

  /// Where the room's nodes go when their parent isn't here: the shared
  /// folder, or the shared space's Pinned.
  var scopeRoot: SidebarNode? {
    switch kind {
    case .folder: rootNode
    case .space: space?.pinned
    case .easel: nil
    }
  }

  /// Where the shared folder (or easel tab) goes: the mount's placement if
  /// it's here, else the end of the account's first space's Pinned.
  func placementParent() -> SidebarNode? {
    let space = store.space(id: mount.spaceUuid).flatMap { $0.isIncognito ? nil : $0 } ?? fallbackSpace()
    guard let space else { return nil }
    if !mount.parentUuid.isEmpty, let parent = store.node(id: mount.parentUuid), parent.isFolder,
      !parent.isLiveFolder, parent.root === space.pinned, !contains(parent)
    {
      return parent
    }
    return space.pinned
  }

  func applyNodes(_ ids: [String]) {
    for id in ids {
      guard let n = mirror[id]?.node, let type = NodeType(rawValue: n.type) else { continue }
      let kind: SidebarNode.Kind =
        switch type {
        case .tab: .tab
        case .folder: .folder
        case .split: .split
        }
      if let node = store.node(id: id) {
        guard node.kind == kind else { continue }
        store.rawUpdate(
          node, title: n.title, customTitle: n.customTitle, url: n.url, lastActive: node.lastActive,
          archivedAt: nil, collapsed: n.collapsed, position: node.position)
        if n.hasSavedURL { store.rawSetSavedURL(node, url: n.savedURL, title: n.savedTitle) }
        if node.isFolder, let source = CompassSpacesBridge.nameSource(proto: n.nameSource) {
          store.rawSetNameSource(node, source)
        }
        if node.isFolder {
          let live = CompassSpacesBridge.liveSource(n)
          if live != node.liveSource, live != nil || node.isLiveFolder {
            if live == nil { for child in node.children where child.isLiveItem { store.rawDetach(child) } }
            store.rawSetLiveSource(node, live)
          }
        }
      } else {
        let node = store.makeNode(
          id: id, kind: kind, title: n.title, customTitle: n.customTitle, url: n.url, collapsed: n.collapsed,
          position: CompassSpacesBridge.position(n.position))
        let parent = id == rootUUID ? placementParent() : (scopeRoot ?? placementParent())
        guard let parent else { continue }
        if kind == .folder, let live = CompassSpacesBridge.liveSource(n) { store.rawSetLiveSource(node, live) }
        if kind == .folder, let source = CompassSpacesBridge.nameSource(proto: n.nameSource) {
          store.rawSetNameSource(node, source)
        }
        store.rawInsert(node, into: parent)
        if n.hasSavedURL { store.rawSetSavedURL(node, url: n.savedURL, title: n.savedTitle) }
      }
    }
  }

  /// Puts every node of the room under its parent, parents first; the
  /// shared folder at its placement.
  func place() {
    let entries = mirror.compactMap { id, s -> (SidebarNode, SyncPb_CompassNode)? in
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
    var touchedSplits: [SidebarNode] = []
    for (node, n) in entries.sorted(by: { depth($0.1) < depth($1.1) }) {
      var position = CompassSpacesBridge.position(n.position)
      let target: SidebarNode
      if kind == .folder && node.id == rootUUID {
        guard let parent = placementParent() else { continue }
        target = parent
        position = mount.hasPosition ? CompassSpacesBridge.position(mount.position) : node.position
        if !position.isValid || (node.parent !== target && !mount.hasPosition) {
          position = UniquePosition.forInsert(
            after: target.children.last?.position, before: nil, suffix: CompassSpacesBridge.suffix(for: node.id))
        }
      } else {
        guard let scopeRoot else { continue }
        var parent = scopeRoot
        if !n.parentUuid.isEmpty, let p = store.node(id: n.parentUuid), contains(p),
          store.canMove(node, to: p) || node.parent === p
        {
          parent = p
        }
        target = parent
      }
      if node.parent !== target || (position.isValid && node.position != position) {
        let old = node.parent
        store.rawDetach(node)
        store.rawUpdate(
          node, title: node.title, customTitle: node.customTitle, url: node.url, lastActive: node.lastActive,
          archivedAt: node.archivedAt, collapsed: node.collapsed,
          position: position.isValid
            ? position
            : UniquePosition.forInsert(
              after: target.children.last?.position, before: nil, suffix: CompassSpacesBridge.suffix(for: node.id)))
        store.rawInsert(node, into: target)
        if n.hasSavedURL { store.rawSetSavedURL(node, url: n.savedURL, title: n.savedTitle) }
        if let old, old.isSplit { touchedSplits.append(old) }
      }
      if target.isSplit, target.children.count > SpaceStore.maxSplitPanes, let scopeRoot {
        store.rawDetach(node)
        store.rawInsert(node, into: scopeRoot)
      }
    }
    for split in touchedSplits { store.rawNormalize(split) }
  }

  /// A shared easel shows as a pinned tab at the mount's placement, unless
  /// the board has a tab here already (the owner's).
  func placeEaselTab() {
    guard kind == .easel, !rootUUID.isEmpty, easels?.easel(rootUUID) != nil else { return }
    let url = EaselURL.url(rootUUID)
    if store.node(id: easelTabID) != nil { return }
    if store.allNodes.contains(where: { $0.isTab && EaselURL.uuid(from: $0.url) == rootUUID }) { return }
    guard let parent = placementParent() else { return }
    let position =
      mount.hasPosition && CompassSpacesBridge.position(mount.position).isValid
      ? CompassSpacesBridge.position(mount.position)
      : UniquePosition.forInsert(
        after: parent.children.last?.position, before: nil, suffix: CompassSpacesBridge.suffix(for: easelTabID))
    let tab = store.makeNode(
      id: easelTabID, kind: .tab, title: easels?.easel(rootUUID)?.title ?? mount.name, url: url, position: position,
      savedURL: url, savedTitle: easels?.easel(rootUUID)?.title ?? mount.name)
    store.rawInsert(tab, into: parent)
  }

  /// A viewer's (or a shared item put back's) local state goes back to the
  /// room's: changed nodes and boards are re-applied, added ones removed.
  func revertLocalEdits() {
    let writes = pendingWrites()
    let missingRoot = !rootUUID.isEmpty && !rootIsHere && !mirror.isEmpty
    guard !writes.isEmpty || missingRoot else { return }
    var changed: [String: SyncPb_CompassSpacesSpecifics] = [:]
    var deleted: Set<String> = []
    for w in writes {
      if let s = mirror[w.id] { changed[w.id] = s } else { deleted.insert(w.id) }
    }
    if missingRoot { changed = mirror }
    // Added here: removed (they aren't in the mirror).
    store.applyingRemote {
      for id in deleted {
        if let node = store.node(id: id), contains(node), id != rootUUID {
          let parent = node.parent
          store.rawDetach(node)
          if let parent, parent.isSplit { store.rawNormalize(parent) }
        }
      }
    }
    // Missing nodes are made again by applying their records.
    for (id, s) in mirror where changed[id] == nil {
      if case .node = s.entity, store.node(id: id) == nil { changed[id] = s }
    }
    apply(changed: changed, deleted: [], force: true)
    if let easels {
      let boardsAdded = deleted.filter { easels.object($0) != nil || easels.easel($0) != nil }
      if !boardsAdded.isEmpty { easels.applyRemote(EaselEntities(), deleted: boardsAdded, force: true) }
    }
  }

  /// Puts the shared item back where it was removed here (a member's Delete
  /// of a shared root isn't a delete for everyone).
  func restoreIfMissing() {
    guard !rootUUID.isEmpty, !rootIsHere, !mirror.isEmpty else { return }
    apply(changed: mirror, deleted: [], force: true)
  }

  // MARK: Keys and members

  /// Reads the room: role, members, epoch, and this account's envelopes
  /// (new keys go into the mount). Whether the mount changed.
  func refresh(_ client: RoomClient, identity: IdentityKey) async throws -> Bool {
    let info = try await client.room(roomID)
    self.info = info
    var mount = self.mount
    for envelope in info.envelopes where mount.keyByEpoch[envelope.epoch] == nil {
      if let key = RoomCrypto.openEnvelope(
        identity: identity, roomID: roomIDBytes, epoch: envelope.epoch, envelope: envelope.sealed)
      {
        mount.setKey(key, epoch: envelope.epoch)
      }
    }
    if mount.role != info.role.rawValue { mount.role = info.role.rawValue }
    let changed = mount != self.mount
    if changed {
      let gotKeys = mount.keys.count != self.mount.keys.count
      self.mount = mount
      onMountChanged?(mount)
      // Records that waited for a key are read again.
      if gotKeys, !state.unreadable.isEmpty {
        state.seq = 0
        state.unreadable = []
      }
    }
    if info.role == .owner { try await sendMissingEnvelopes(client) }
    return changed
  }

  /// The owner: every joined member gets the keys of every epoch (one who
  /// joined by a link made before a rotation has only that link's).
  func sendMissingEnvelopes(_ client: RoomClient) async throws {
    guard let info, info.role == .owner, info.epoch > 1 else { return }
    let keys = self.keys
    var list: [RoomEnvelopeFor] = []
    var sentTo: [String] = []
    for member in info.members where member.role != .owner && member.joined > 0 {
      guard (state.envelopesSent[member.clientID] ?? 0) < info.epoch else { continue }
      for (epoch, key) in keys where epoch <= info.epoch {
        if let sealed = try? RoomCrypto.sealEnvelope(
          recipientPublicKey: member.identityPub, roomID: roomIDBytes, epoch: epoch, roomKey: key)
        {
          list.append(RoomEnvelopeFor(clientID: member.clientID, epoch: epoch, sealed: sealed))
        }
      }
      sentTo.append(member.clientID)
    }
    guard !list.isEmpty else { return }
    try await client.addEnvelopes(roomID, list)
    for id in sentTo { state.envelopesSent[id] = info.epoch }
    save()
  }

  /// The owner: the next epoch, its key sealed to every current member.
  func rotate(_ client: RoomClient, identity: IdentityKey) async throws {
    for attempt in 0..<3 {
      let info = try await client.room(roomID)
      self.info = info
      let next = info.epoch + 1
      let key = RoomCrypto.generateRoomKey()
      var envelopes: [String: Data] = [:]
      for member in info.members {
        envelopes[member.clientID] = try RoomCrypto.sealEnvelope(
          recipientPublicKey: member.identityPub, roomID: roomIDBytes, epoch: next, roomKey: key)
      }
      do {
        try await client.rotate(roomID, epoch: next, envelopes: envelopes)
      } catch RoomError.envelopesMismatch where attempt < 2 {
        continue
      } catch RoomError.staleEpoch where attempt < 2 {
        continue
      }
      mount.setKey(key, epoch: next)
      for member in info.members { state.envelopesSent[member.clientID] = next }
      save()
      onMountChanged?(mount)
      self.info = try await client.room(roomID)
      return
    }
  }
}
