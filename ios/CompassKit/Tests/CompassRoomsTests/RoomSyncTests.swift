// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassEasels
import CompassModel
@testable import CompassSync
import Foundation
import SyncProto
import Testing

@testable import CompassRooms

/// A device of one chain: the sidebar, its bridge (engine against
/// `server`), the Easels and the rooms.
@MainActor
final class RoomDevice {
  let store = SpaceStore()
  let directory: SpaceDirectory
  let engine: SyncEngine
  let bridge: CompassSpacesBridge
  let easels = EaselStore(directory: nil)
  let rooms: RoomsManager
  let account: RoomsManager.Account
  let chain: Chain

  init(chain: Chain, server: URL = URL(string: "http://127.0.0.1:9/v2")!, first: Bool = true, name: String = "") {
    self.chain = chain
    let metadata = SyncMetadata()
    engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server, cacheGUID: metadata.cacheGUID, authKey: chain.authKey),
      passphrase: chain.passphrase, store: MemorySyncMetadataStore(metadata), createChainIfEmpty: first)
    directory = SpaceDirectory(store: store)
    bridge = CompassSpacesBridge(directory: directory, profile: "", engine: engine, mirrorURL: nil)
    rooms = RoomsManager(directory: directory, easels: easels, stateDirectory: nil, displayName: name)
    let easels = self.easels
    bridge.localEasels = { easels.entities() }
    bridge.easelAssetData = { easels.assetData($0) }
    bridge.onEasels = { changed, deleted in easels.applyRemote(changed, deleted: deleted) }
    easels.onChange = { [weak bridge] in bridge?.easelsChanged(easels.entities()) }
    account = RoomsManager.Account(
      profile: "", bridge: bridge, serverURL: server, authKey: chain.authKey,
      identity: RoomCrypto.identityKey(chain: chain), cacheGUID: metadata.cacheGUID)
    rooms.addAccount(account)
  }

  /// With the engine registered (server tests).
  static func make(chain: Chain, server: URL, first: Bool = true, name: String = "") async -> RoomDevice {
    let device = RoomDevice(chain: chain, server: server, first: first, name: name)
    await device.engine.register(device.bridge)
    return device
  }

  func sync() async throws {
    await bridge.flush()
    try await engine.sync()
    await bridge.flush()
    if await engine.hasPendingChanges { try await engine.sync() }
    await bridge.flush()
  }
}

/// Records of a room, as the server would hand them out.
func sealed(
  _ entities: [String: SyncPb_CompassSpacesSpecifics], key: Data, roomID: Data, epoch: UInt32 = 1, seq: Int64 = 1
) -> [RoomRecord] {
  var seq = seq
  return entities.keys.sorted().map { id in
    defer { seq += 1 }
    let plain = try! entities[id]!.serializedBytes() as Data
    return RoomRecord(
      id: id, seq: seq, epoch: epoch, deleted: false,
      ciphertext: try! RoomCrypto.encryptRecord(roomKey: key, roomID: roomID, recordID: id, epoch: epoch, plaintext: plain))
  }
}

func folderMount(root: String, key: Data, roomID: Data, role: RoomRole = .editor, space: String = "", parent: String = "")
  -> RoomMount
{
  var m = RoomMount()
  m.roomID = roomID.base64URLEncodedString(padded: false)
  m.serverURL = "http://127.0.0.1:9/v2"
  m.kind = "folder"
  m.rootUuid = root
  m.role = role.rawValue
  m.setKey(key, epoch: 1)
  if !space.isEmpty { m.spaceUuid = space }
  if !parent.isEmpty { m.parentUuid = parent }
  return m
}

@MainActor
struct RoomSyncTests {
  let key = RoomCrypto.generateRoomKey()
  let roomID = RoomCrypto.generateRoomID()

  /// Alice's sidebar: Work with a folder Team (a tab, and Inner with a tab)
  /// and a pinned tab outside it.
  func alice() -> (RoomDevice, SidebarNode, SidebarNode) {
    let device = RoomDevice(chain: try! Chain(seed: Chain.generateSeed()))
    let work = device.store.addSpace(name: "Work")
    let team = device.store.addFolder(name: "Team", to: work.pinned)!
    device.store.addTab(url: "https://a.example/", title: "A", to: team)
    let inner = device.store.addFolder(name: "Inner", to: team)!
    device.store.addTab(url: "https://b.example/", title: "B", to: inner)
    let outside = device.store.addTab(url: "https://out.example/", title: "Out", to: work.pinned)!
    _ = device.bridge.mergeWithChain()
    return (device, team, outside)
  }

  @Test func recordsAreTheSharedSubtreeOnly() throws {
    let (a, team, outside) = alice()
    let room = a.rooms.start(folderMount(root: team.id, key: key, roomID: roomID, role: .owner), profile: "")
    let want = room.desired()
    #expect(Set(want.keys) == Set(team.subtree.map(\.id)))
    let root = want[team.id]!.node
    #expect(!root.hasParentUuid && !root.hasPosition && !root.hasSpaceUuid)
    #expect(root.section == Section.pinned.rawValue && root.title == "Team")
    for (id, s) in want where id != team.id {
      #expect(!s.node.hasSpaceUuid)
      #expect(!s.node.parentUuid.isEmpty)
    }
    // The chain leaves the shared folder to the room; the rest syncs.
    #expect(a.bridge.desired(team) == nil)
    #expect(a.bridge.desired(team.children[0]) == nil)
    #expect(a.bridge.desired(outside) != nil)
    #expect(a.rooms.isRoomNodeID(team.children[1].children[0].id))
    #expect(!a.rooms.isRoomNodeID(outside.id))
    // Everything is to write, nothing to delete.
    #expect(room.pendingWrites().count == 4 && room.pendingWrites().allSatisfy { $0.specifics != nil && $0.base == 0 })
  }

  @Test func appliesAtThePlacementAndRefusesForeignNodes() throws {
    let (a, team, _) = alice()
    let records = a.rooms.start(folderMount(root: team.id, key: key, roomID: roomID, role: .owner), profile: "").desired()

    let b = RoomDevice(chain: try Chain(seed: Chain.generateSeed()))
    let home = b.store.addSpace(name: "Home")
    let projects = b.store.addFolder(name: "Projects", to: home.pinned)!
    let mine = b.store.addTab(url: "https://mine.example/", title: "Mine", to: home.pinned)!
    _ = b.bridge.mergeWithChain()
    let room = b.rooms.start(
      folderMount(root: team.id, key: key, roomID: roomID, space: home.id, parent: projects.id), profile: "")
    // A record that would take over one of Bob's own tabs is skipped.
    var evil = SyncPb_CompassSpacesSpecifics()
    evil.node.uuid = mine.id
    evil.node.parentUuid = team.id
    evil.node.section = Section.pinned.rawValue
    evil.node.type = NodeType.tab.rawValue
    evil.node.url = "https://evil.example/"
    var all = records
    all[mine.id] = evil
    let (changed, deleted) = room.ingest(sealed(all, key: key, roomID: roomID))
    room.apply(changed: changed, deleted: deleted)
    #expect(room.rootUuidIsSet)
    let shared = try #require(b.store.node(id: team.id))
    #expect(shared.parent === projects)
    #expect(shared.children.map(\.displayTitle) == ["A", "Inner"])
    #expect(shared.children[1].children.first?.url == "https://b.example/")
    #expect(mine.url == "https://mine.example/" && mine.parent === home.pinned)
    #expect(b.rooms.isRoomNode(shared.children[0]))
    // Nothing to write back: it's the room's.
    #expect(room.pendingWrites().isEmpty)
    // A record with a tampered epoch doesn't read.
    var bad = sealed([team.id: records[team.id]!], key: key, roomID: roomID, seq: 50)[0]
    bad.epoch = 2
    #expect(room.ingest([bad]).changed.isEmpty)
  }

  @Test func editsDiffAgainstTheMirror() throws {
    let (a, team, _) = alice()
    let room = a.rooms.start(folderMount(root: team.id, key: key, roomID: roomID, role: .owner), profile: "")
    _ = room.ingest(sealed(room.desired(), key: key, roomID: roomID))
    #expect(room.pendingWrites().isEmpty)
    a.store.rename(team, customTitle: "Team 2")
    let inner = team.children[1]
    let innerTab = inner.children[0]
    a.store.remove(inner)
    let writes = room.pendingWrites()
    #expect(Set(writes.map(\.id)) == [team.id, inner.id, innerTab.id])
    #expect(writes.first { $0.id == team.id }?.specifics?.node.title == "Team 2")
    // The removed folder and its tab: deletions, based on their seq.
    #expect(writes.filter { $0.specifics == nil }.count == 2)
    #expect(writes.allSatisfy { $0.base > 0 })
    // The shared folder itself is never deleted by a diff: gone here, it
    // writes nothing and comes back.
    a.store.remove(team)
    #expect(room.pendingWrites().isEmpty)
    room.restoreIfMissing()
    #expect(a.store.node(id: team.id) != nil)
  }

  @Test func aViewersEditsGoBack() throws {
    let (a, team, _) = alice()
    let records = a.rooms.start(folderMount(root: team.id, key: key, roomID: roomID, role: .owner), profile: "").desired()
    let b = RoomDevice(chain: try Chain(seed: Chain.generateSeed()))
    let home = b.store.addSpace(name: "Home")
    _ = b.bridge.mergeWithChain()
    let room = b.rooms.start(
      folderMount(root: team.id, key: key, roomID: roomID, role: .viewer, space: home.id), profile: "")
    let (changed, deleted) = room.ingest(sealed(records, key: key, roomID: roomID))
    room.apply(changed: changed, deleted: deleted)
    let shared = try #require(b.store.node(id: team.id))
    let tab = shared.children[0]
    b.store.rename(tab, customTitle: "Mine now")
    #expect(tab.customTitle == "")
    let added = b.store.addTab(url: "https://added.example/", title: "Added", to: shared)
    #expect(added != nil && b.store.node(id: added!.id) == nil)
    b.store.remove(shared.children[1])
    #expect(b.store.node(id: shared.id)?.children.count == 2)
    #expect(!b.rooms.canEdit(tab))
  }

  @Test func sharedEaselsLeaveThePersonalSync() throws {
    let a = RoomDevice(chain: try Chain(seed: Chain.generateSeed()))
    a.store.addSpace(name: "Work")
    _ = a.bridge.mergeWithChain()
    let board = try a.easels.createEasel(title: "Plan").get()
    var text = EaselObject()
    text.uuid = UUID().uuidString.lowercased()
    text.easelUuid = board.uuid
    text.text.text = "Hello"
    _ = try a.easels.save(text).get()
    let other = try a.easels.createEasel(title: "Mine").get()
    var mount = folderMount(root: board.uuid, key: key, roomID: roomID, role: .owner)
    mount.kind = "easel"
    let room = a.rooms.start(mount, profile: "")
    #expect(Set(room.desired().keys) == [board.uuid, text.uuid])
    let personal = a.bridge.easelSpecifics(a.easels.entities()).compactMap { CompassSpacesBridge.uuid(of: $0) }
    #expect(personal == [other.uuid])
  }
}

extension RoomSync {
  var rootUuidIsSet: Bool { !rootUUID.isEmpty }
}
