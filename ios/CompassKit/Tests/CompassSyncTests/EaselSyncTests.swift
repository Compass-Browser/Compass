// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassEasels
import CompassModel
@testable import CompassSync
import Foundation
import SwiftProtobuf
import SyncProto
import Testing

/// Easels on the wire (docs/design/easels.md, "Sync"): a board, each object
/// and each image their own entity in the home account; images sent with
/// their bytes once, only when the account doesn't have them.
@MainActor
struct EaselSyncTests {
  let easels = EaselStore(directory: nil)

  private func bridge() -> CompassSpacesBridge {
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let bridge = CompassSpacesBridge(store: SpaceStore(), engine: engine, mirrorURL: nil)
    wire(bridge)
    _ = bridge.mergeWithChain()
    return bridge
  }

  private func wire(_ bridge: CompassSpacesBridge) {
    let easels = easels
    bridge.localEasels = { easels.entities() }
    bridge.easelAssetData = { easels.assetData($0) }
    bridge.onEasels = { changed, deleted in easels.applyRemote(changed, deleted: deleted) }
    easels.onChange = { [weak bridge] in bridge?.easelsChanged(easels.entities()) }
  }

  private func change(_ s: SyncPb_CompassSpacesSpecifics) -> RemoteChange {
    let id = CompassSpacesBridge.uuid(of: s)!
    var e = SyncPb_EntitySpecifics()
    e.compassSpaces = s
    return RemoteChange(
      clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: id), specifics: e, serverID: id, version: 1,
      mtime: 0, ctime: 0, originatorCacheGUID: "mac")
  }

  private func deletion(_ id: String) -> RemoteChange {
    RemoteChange(
      clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: id), specifics: nil, serverID: id, version: 9,
      mtime: 0, ctime: 0, originatorCacheGUID: "mac")
  }

  private func note(_ easel: String) -> EaselObject {
    var o = EaselObject()
    o.easelUuid = easel
    o.width = 100
    o.height = 40
    o.text.text = "Hi"
    return o
  }

  @Test func theWireShape() throws {
    var s = SyncPb_CompassSpacesSpecifics()
    s.easel.uuid = "0f8fad5b-d9cb-469f-a165-70867728950e"
    // CompassSpacesSpecifics field 8 (length-delimited): tag 0x42; 9: 0x4a;
    // 10: 0x52.
    #expect(([UInt8](try s.serializedData())).first == 0x42)
    s.easelObject.uuid = s.easel.uuid
    #expect(([UInt8](try s.serializedData())).first == 0x4a)
    s.easelAsset.uuid = s.easelObject.uuid
    #expect(([UInt8](try s.serializedData())).first == 0x52)
    #expect(CompassSpacesBridge.uuid(of: s) == s.easelAsset.uuid)
  }

  @Test func theInitialMergeSendsLocalEasels() throws {
    let board = try easels.createEasel(title: "Trip").get()
    let sha = try easels.addAsset(Data("img".utf8), mimeType: "image/png").get()
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let bridge = CompassSpacesBridge(store: SpaceStore(), engine: engine, mirrorURL: nil)
    wire(bridge)
    let merged = bridge.mergeWithChain()
    #expect(merged.contains { $0.tag == board.uuid && $0.specifics.compassSpaces.easel.title == "Trip" })
    let asset = try #require(merged.first { $0.tag == Easels.assetUUID(sha256: sha) })
    // Sent with its bytes; the mirror keeps it without them.
    #expect(asset.specifics.compassSpaces.easelAsset.data == Data("img".utf8))
    #expect(bridge.mirror[asset.tag]?.easelAsset.hasData == false)
  }

  @Test func localChangesCommitAndDelete() async throws {
    let b = bridge()
    let board = try easels.createEasel(title: "B").get()
    let object = try easels.save(note(board.uuid)).get()
    #expect(b.mirror[board.uuid]?.easel.title == "B")
    #expect(b.mirror[object.uuid]?.easelObject.text.text == "Hi")
    await b.flush()
    #expect(await b.engine.knownTags(.compassSpaces).contains(object.uuid))
    // Deleting here deletes the entity.
    easels.deleteObject(object.uuid)
    #expect(b.mirror[object.uuid] == nil)
    await b.flush()
    #expect(await !b.engine.knownTags(.compassSpaces).contains(object.uuid))
  }

  @Test func imagesGoOnceWithTheirBytes() async throws {
    let b = bridge()
    let sha = try easels.addAsset(Data("png".utf8), mimeType: "image/png").get()
    let id = Easels.assetUUID(sha256: sha)
    await b.flush()
    #expect(await b.engine.knownTags(.compassSpaces).contains(id))
    #expect(b.mirror[id]?.easelAsset.sha256 == sha)
    #expect(b.mirror[id]?.easelAsset.hasData == false)
    // Another change doesn't send it again.
    let lastSent = b.lastCommit[id]
    _ = try easels.createEasel(title: "Later").get()
    #expect(b.lastCommit[id] == lastSent)
    // Nor does one the account already had.
    var s = SyncPb_CompassSpacesSpecifics()
    var asset = EaselAsset()
    asset.sha256 = Easels.sha256Hex(Data("theirs".utf8))
    asset.uuid = Easels.assetUUID(sha256: asset.sha256)
    asset.mimeType = "image/png"
    asset.data = Data("theirs".utf8)
    s.easelAsset = asset
    b.applyRemote([change(s)])
    #expect(easels.assetData(asset.sha256) == Data("theirs".utf8))
    #expect(b.lastCommit[asset.uuid] == nil)
    #expect(b.mirror[asset.uuid]?.easelAsset.hasData == false)
  }

  @Test func remoteChangesArrive() throws {
    let b = bridge()
    var board = EaselBoard()
    board.uuid = "0f8fad5b-d9cb-469f-a165-70867728950e"
    board.title = "Theirs"
    board.updateTime = 5
    var s = SyncPb_CompassSpacesSpecifics()
    s.easel = board
    var object = note(board.uuid)
    object.uuid = "11111111-1111-4111-8111-111111111111"
    object.updateTime = 5
    var o = SyncPb_CompassSpacesSpecifics()
    o.easelObject = object
    b.applyRemote([change(s), change(o)])
    #expect(easels.easel(board.uuid)?.title == "Theirs")
    #expect(easels.objects(of: board.uuid).count == 1)
    // Applying it commits nothing back.
    #expect(b.lastCommit[board.uuid] == nil)
    b.applyRemote([deletion(object.uuid)])
    #expect(easels.object(object.uuid) == nil)
    b.applyRemote([deletion(board.uuid)])
    #expect(easels.easel(board.uuid) == nil)
  }

  /// An older remote edit loses: the local one is sent back.
  @Test func laterEditWins() throws {
    let b = bridge()
    let board = try easels.createEasel(title: "Mine").get()
    var older = board
    older.title = "Older"
    older.updateTime = board.updateTime - 1
    var s = SyncPb_CompassSpacesSpecifics()
    s.easel = older
    b.applyRemote([change(s)])
    #expect(easels.easel(board.uuid)?.title == "Mine")
  }

  /// Other profiles' accounts don't carry the phone's Easels.
  @Test func onlyTheHomeAccount() throws {
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let store = SpaceStore()
    let home = CompassSpacesBridge(store: store, engine: engine, mirrorURL: nil)
    let other = CompassSpacesBridge(
      directory: home.directory, profile: "22222222-2222-4222-8222-222222222222", engine: engine, mirrorURL: nil)
    _ = other.mergeWithChain()
    _ = try easels.createEasel(title: "B").get()
    other.easelsChanged(easels.entities())
    #expect(other.mirror.values.allSatisfy { !CompassSpacesBridge.isEasel($0) })
  }
}
