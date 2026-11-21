// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassEasels
import CompassModel
import CryptoKit
import Foundation
import SyncProto

/// What's shared with other people (docs/design/sync.md, "Rooms"; CompassRooms'
/// RoomsManager on the phone). A space, a pinned folder or an easel shared
/// in a room syncs through the room, encrypted with its keys, and never
/// through the chain: every bridge leaves it out (as a space that stays on
/// the device), and doesn't apply or delete it from remote changes either.
@MainActor
public protocol RoomBacking: AnyObject {
  /// Whether a space's own entity (name, theme) comes from a room: a shared
  /// space. Its Today and archive still sync in the account.
  func isRoomSpace(_ id: String) -> Bool
  /// Whether a node comes from a room: a shared folder and what's in it, a
  /// shared space's Pinned, a shared easel's tab.
  func isRoomNode(_ node: SidebarNode) -> Bool
  /// The same for a node ID that may not be in the store (yet).
  func isRoomNodeID(_ id: String) -> Bool
  /// Whether an easel board, object or image (by its UUID) comes from a room.
  func isRoomEasel(_ id: String) -> Bool
  /// The room mounts of `bridge`'s account changed (another device joined,
  /// left or moved one): called before the rest of a remote batch is
  /// applied, so the shared items it brings are known as such.
  func mountsChanged(_ bridge: CompassSpacesBridge)
}

/// Room mounts in COMPASS_SPACES (`room_mount`): one per room the account
/// is a member of, so every device of the chain joins it, with the keys to
/// read it and where it shows. The client tag is `mountTag(roomID:)`.
extension CompassSpacesBridge {
  @MainActor var rooms: (any RoomBacking)? { directory.rooms }

  @MainActor func isRoomNode(_ node: SidebarNode) -> Bool { rooms?.isRoomNode(node) ?? false }
  @MainActor func isRoomSpace(_ id: String) -> Bool { rooms?.isRoomSpace(id) ?? false }
  @MainActor func isRoomEasel(_ id: String) -> Bool { rooms?.isRoomEasel(id) ?? false }

  /// Whether a remote space or node is a room's.
  @MainActor
  func isRoomEntity(id: String, _ specifics: SyncPb_CompassSpacesSpecifics?) -> Bool {
    guard let rooms else { return false }
    if rooms.isRoomSpace(id) {
      // The space entity itself (not a Today node with the same ID: none).
      if case .node? = specifics?.entity { return false }
      return true
    }
    if rooms.isRoomNodeID(id) { return true }
    if case .node(let n)? = specifics?.entity, !n.parentUuid.isEmpty, rooms.isRoomNodeID(n.parentUuid) { return true }
    return false
  }

  /// Easels without the shared ones.
  @MainActor
  func personalEasels(_ easels: EaselEntities) -> EaselEntities {
    guard let rooms else { return easels }
    var out = easels
    out.easels.removeAll { rooms.isRoomEasel($0.uuid) }
    out.objects.removeAll { rooms.isRoomEasel($0.uuid) || rooms.isRoomEasel($0.easelUuid) }
    out.assets.removeAll { rooms.isRoomEasel($0.uuid) }
    return out
  }

  // MARK: Mounts

  /// The mount's client tag: UUID-shaped, from the room ID (the proto's
  /// rule, as an Easel image's from its hash).
  public static func mountTag(roomID: String) -> String {
    var b = Array(SHA256.hash(data: Data(("compass-room-mount:" + roomID).utf8)).prefix(16))
    b[6] = (b[6] & 0x0f) | 0x50
    b[8] = (b[8] & 0x3f) | 0x80
    let hex = b.map { String(format: "%02x", $0) }.joined()
    let c = Array(hex)
    return [c[0..<8], c[8..<12], c[12..<16], c[16..<20], c[20..<32]].map { String($0) }.joined(separator: "-")
  }

  /// The account's mounts, as the server has them (or as this device last
  /// wrote them).
  @MainActor
  public func roomMounts() -> [SyncPb_CompassRoomMount] {
    mirror.values.compactMap { s -> SyncPb_CompassRoomMount? in
      guard case .roomMount(let m) = s.entity, !m.roomID.isEmpty else { return nil }
      return m
    }.sorted { $0.roomID < $1.roomID }
  }

  /// Joins (or updates) a mount: every device of the account follows.
  @MainActor
  public func putRoomMount(_ mount: SyncPb_CompassRoomMount) {
    var s = SyncPb_CompassSpacesSpecifics()
    s.roomMount = mount
    commit(Self.mountTag(roomID: mount.roomID), s)
    drain()
  }

  /// Leaves a mount (left, removed, or the room was deleted).
  @MainActor
  public func deleteRoomMount(roomID: String) {
    let tag = Self.mountTag(roomID: roomID)
    guard mirror[tag] != nil else { return }
    mirror[tag] = nil
    lastCommit[tag] = nil
    queueDelete(tag)
    drain()
  }

  // MARK: Shared items coming and going

  /// What's shared changed here (a share, a join, a leave, a removal): the
  /// account deletes what's now a room's, and takes back what isn't any
  /// more (a removed member keeps its copy, unshared). Nodes that waited
  /// for a shared space that's here now are applied.
  @MainActor
  public func roomsChanged() {
    guard tracking else { return }
    store.applyingRemote {
      let waiting = mirror.compactMap { id, s -> String? in
        guard case .node(let n) = s.entity, store.node(id: id) == nil, n.section != Section.favorites.rawValue,
          ownsSpace(id: n.spaceUuid), !isLocalSpace(id: n.spaceUuid), !isRoomEntity(id: id, s)
        else { return nil }
        return id
      }
      if !waiting.isEmpty {
        applyNodes(waiting)
        place()
      }
    }
    for space in store.spaces where owns(space) && space.synced {
      if isRoomSpace(space.id) {
        deleteFromServer([space.id])
      } else if mirror[space.id] == nil {
        commit(space.id, desired(space))
      }
    }
    for node in store.allNodes where owns(node) {
      if isLocal(node) {
        if mirror[node.id] != nil, isRoomNode(node) { deleteFromServer([node.id]) }
      } else if mirror[node.id] == nil, let specifics = desired(node) {
        commit(node.id, specifics)
      }
    }
    if isHome, let easels = localEasels?() {
      easelsChanged(easels)
    } else {
      drain()
    }
  }
}
