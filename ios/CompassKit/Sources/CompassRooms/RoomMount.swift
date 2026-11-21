// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
import CompassSync
import CryptoKit
import Foundation
import SyncProto

/// A room mount (`CompassRoomMount`, carried in COMPASS_SPACES by the
/// account that's a member): the keys and where the shared item shows.
public typealias RoomMount = SyncPb_CompassRoomMount

extension SyncPb_CompassRoomMount {
  /// The room key of each epoch this account has.
  public var keyByEpoch: [UInt32: Data] {
    var out: [UInt32: Data] = [:]
    for k in keys where k.epoch > 0 && k.key.count == RoomCrypto.roomKeyLength { out[k.epoch] = k.key }
    return out
  }

  /// Adds (or replaces) an epoch's key; keys stay ordered by epoch.
  public mutating func setKey(_ key: Data, epoch: UInt32) {
    var all = keyByEpoch
    all[epoch] = key
    keys = all.keys.sorted().map { e in
      var k = SyncPb_CompassRoomKey()
      k.epoch = e
      k.key = all[e]!
      return k
    }
  }

  public var roomKind: RoomKind? { RoomKind(rawValue: kind) }
  public var roomRole: RoomRole { RoomRole(rawValue: role) ?? .viewer }
  public var roomIDBytes: Data? { RoomInviteLink.unb64(roomID).flatMap { $0.count == RoomCrypto.roomIDLength ? $0 : nil } }

  /// A mount this client can use.
  public var isUsable: Bool {
    roomIDBytes != nil && roomKind != nil && URL(string: serverURL) != nil
  }
}

/// The record format of rooms (docs/design/ios.md, "Rooms"): a record is a
/// serialized CompassSpacesSpecifics under the entity's UUID, as in the
/// chain, with what's personal left out.
///
/// - folder: a `node` per folder, tab and split of the subtree, section
///   PINNED, no `space_uuid`; the shared folder itself has no
///   `parent_uuid` and no `position` (each member places it: the mount).
/// - space: one `space` (uuid, name, theme; no profile, list position or
///   archiving) and a `node` per entry of its Pinned, with its
///   `space_uuid`.
/// - easel: one `easel`, its `easel_object`s, and the `easel_asset`s they
///   show (with their bytes).
///
/// Boards that tabs of a shared folder or space show are shared with it.
public enum RoomRecords {
  /// A shared easel's tab: UUID-shaped, from the room ID (the same on every
  /// device of the member, which never syncs it).
  public static func easelTabID(roomID: String) -> String {
    var b = Array(SHA256.hash(data: Data(("compass-room-tab:" + roomID).utf8)).prefix(16))
    b[6] = (b[6] & 0x0f) | 0x50
    b[8] = (b[8] & 0x3f) | 0x80
    let hex = b.map { String(format: "%02x", $0) }.joined()
    let c = Array(hex)
    return [c[0..<8], c[8..<12], c[12..<16], c[16..<20], c[20..<32]].map { String($0) }.joined(separator: "-")
  }

  /// The ID a record must have: its entity's UUID. Entities rooms don't
  /// carry (profiles, memories, Boosts, mounts…) have none.
  public static func recordID(of s: SyncPb_CompassSpacesSpecifics) -> String? {
    switch s.entity {
    case .space(let x): x.uuid
    case .node(let x): x.uuid
    case .easel(let x): x.uuid
    case .easelObject(let x): x.uuid
    case .easelAsset(let x): x.uuid
    default: nil
    }
  }

  /// The shared item's UUID among a room's records: the space, the folder
  /// without a parent, the board. Nil until it's there.
  public static func rootUUID(kind: RoomKind, records: [String: SyncPb_CompassSpacesSpecifics]) -> String? {
    switch kind {
    case .space:
      return records.values.compactMap { s -> String? in
        if case .space(let x) = s.entity { return x.uuid }
        return nil
      }.min()
    case .folder:
      return records.values.compactMap { s -> String? in
        if case .node(let n) = s.entity, n.parentUuid.isEmpty, n.type == NodeType.folder.rawValue { return n.uuid }
        return nil
      }.min()
    case .easel:
      return records.values.compactMap { s -> String? in
        if case .easel(let x) = s.entity { return x.uuid }
        return nil
      }.min()
    }
  }
}

/// A member's self-chosen name (`profile`), readable by the room's members:
/// epoch (big-endian UInt32) || the record encryption of `{"name": …}`
/// under that epoch's key, as the record "profile:<chain id>".
public enum RoomProfile {
  public static func seal(name: String, chainID: String, roomID: Data, epoch: UInt32, key: Data) -> Data? {
    guard let json = try? JSONSerialization.data(withJSONObject: ["name": name], options: [.sortedKeys]),
      let sealed = try? RoomCrypto.encryptRecord(
        roomKey: key, roomID: roomID, recordID: "profile:" + chainID.lowercased(), epoch: epoch, plaintext: json)
    else { return nil }
    let out = RoomCrypto.bigEndian(epoch) + sealed
    return out.count <= 4096 ? out : nil
  }

  public static func open(_ data: Data, chainID: String, roomID: Data, keys: [UInt32: Data]) -> String? {
    guard data.count > 4 + RoomCrypto.recordOverhead else { return nil }
    let epoch = data.prefix(4).reduce(UInt32(0)) { $0 << 8 | UInt32($1) }
    guard let key = keys[epoch],
      let plain = RoomCrypto.decryptRecord(
        roomKey: key, roomID: roomID, recordID: "profile:" + chainID.lowercased(), epoch: epoch,
        sealed: Data(data.dropFirst(4))),
      let json = try? JSONSerialization.jsonObject(with: plain) as? [String: Any],
      let name = json["name"] as? String
    else { return nil }
    return name
  }
}
