// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
@testable import CompassSync
import Foundation
import SwiftProtobuf
import SyncProto
import Testing

struct NoConnection: SyncConnection {
  func post(_ message: SyncPb_ClientToServerMessage) async throws -> SyncPb_ClientToServerResponse {
    throw SyncConnectionError.network("offline")
  }
}

@MainActor
struct CompassSpacesBridgeTests {
  func make() -> (SpaceStore, CompassSpacesBridge, SyncEngine) {
    let store = SpaceStore()
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let bridge = CompassSpacesBridge(store: store, engine: engine, mirrorURL: nil)
    _ = bridge.mergeWithChain()  // tracking from here on
    return (store, bridge, engine)
  }

  static func uuid() -> String { UUID().uuidString.lowercased() }

  func remote(_ build: (inout SyncPb_CompassSpacesSpecifics) -> Void) -> RemoteChange {
    var s = SyncPb_CompassSpacesSpecifics()
    build(&s)
    var e = SyncPb_EntitySpecifics()
    e.compassSpaces = s
    let id = CompassSpacesBridge.uuid(of: s)!
    return RemoteChange(
      clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: id), specifics: e, serverID: id, version: 1,
      mtime: 0, ctime: 0, originatorCacheGUID: "mac")
  }

  func space(_ id: String, _ name: String) -> RemoteChange {
    remote {
      $0.space.uuid = id
      $0.space.name = name
      $0.space.color = 3
      $0.space.archiveAfterSeconds = 0
    }
  }

  func node(
    _ id: String, space: String, parent: String = "", section: Section = .pinned, type: NodeType = .tab,
    url: String = "", title: String = "", position: UniquePosition? = nil
  ) -> RemoteChange {
    remote {
      $0.node.uuid = id
      $0.node.spaceUuid = space
      if !parent.isEmpty { $0.node.parentUuid = parent }
      $0.node.section = section.rawValue
      $0.node.type = type.rawValue
      if !url.isEmpty { $0.node.url = url }
      if !title.isEmpty { $0.node.title = title }
      let p = position ?? UniquePosition.initial(suffix: CompassSpacesBridge.suffix(for: id))
      $0.node.position = CompassSpacesBridge.proto(p)
    }
  }

  @Test func clientTagHashMatchesTheDesktop() {
    #expect(ClientTagHash.make(type: .compassSpaces, tag: "0f6d3f4e-1111-4222-8333-444444444444")
      == "C8HbbahHD/9jkO3SUxgcFwf7lVU=")
  }

  @Test func windowsEpoch() {
    let date = Date(timeIntervalSince1970: 1_700_000_000.5)
    #expect(CompassSpacesBridge.windowsMicros(date) == 13_344_473_600_500_000)
    #expect(CompassSpacesBridge.date(windowsMicros: 13_344_473_600_500_000) == date)
  }

  @Test func nodeWireFormat() {
    let (store, bridge, _) = make()
    let space = store.addSpace(name: "Work")
    let folder = store.addFolder(name: "Docs", to: space.pinned)!
    let tab = store.addTab(url: "https://a.example/", title: "A", to: folder)!
    let fav = store.addTab(url: "https://fav.example/", to: store.favorites)!
    let today = store.addTab(url: "https://t.example/", to: space.today, now: Date(timeIntervalSince1970: 1000))!

    let f = bridge.desired(folder)!.node
    #expect(f.type == 2 && f.title == "Docs" && !f.hasURL && !f.hasCollapsed && !f.hasParentUuid)
    #expect(f.spaceUuid == space.id && f.section == 1)
    let t = bridge.desired(tab)!.node
    #expect(t.parentUuid == folder.id && t.url == "https://a.example/" && !t.hasLastActiveTime)
    #expect(CompassSpacesBridge.position(t.position) == tab.position)
    let v = bridge.desired(fav)!.node
    #expect(!v.hasSpaceUuid && v.section == 3)
    let d = bridge.desired(today)!.node
    #expect(d.section == 2 && d.lastActiveTime == CompassSpacesBridge.windowsMicros(Date(timeIntervalSince1970: 1000)))
    let s = bridge.desired(space).space
    #expect(s.color == SpaceColor.blue.rawValue && s.archiveAfterSeconds == 43200 && s.hasDirectoryPosition)

    // Positions use the entity's own suffix.
    #expect(Array(tab.position.bytes.suffix(28)) == CompassSpacesBridge.suffix(for: tab.id))
  }

  @Test func newTabPagesStayLocal() {
    let (store, bridge, _) = make()
    let space = store.addSpace(name: "Work")
    let blank = store.addTab(url: "about:blank", to: space.today)!
    #expect(bridge.desired(blank) == nil)
  }

  @Test func appliesATreeAndParksOrphans() async {
    let (store, bridge, _) = make()
    let s = Self.uuid(), folder = Self.uuid(), child = Self.uuid(), orphan = Self.uuid()
    bridge.applyRemote([
      space(s, "Work"),
      node(child, space: s, parent: folder, url: "https://c/"),
      node(orphan, space: s, parent: Self.uuid(), url: "https://o/"),
      node(folder, space: s, type: .folder, title: "F"),
    ])
    let space = store.space(id: s)!
    #expect(space.name == "Work" && space.theme == SpaceTheme(.purple) && space.archiveAfter == nil)
    #expect(store.node(id: child)?.parent?.id == folder)
    // The orphan waits at the root, and nothing is committed for it.
    #expect(store.node(id: orphan)?.parent === space.pinned)
    #expect(bridge.parked.contains(orphan))
    await bridge.flush()
    #expect(await bridge.engine.hasPendingChanges == false)
  }

  @Test func cycleGoesToTheRootAndCommitsTheFix() async {
    let (store, bridge, engine) = make()
    let s = Self.uuid(), a = Self.uuid(), b = Self.uuid()
    bridge.applyRemote([
      space(s, "W"), node(a, space: s, type: .folder, title: "A"), node(b, space: s, parent: a, type: .folder),
    ])
    // Another device moved A into B: a cycle here.
    bridge.applyRemote([node(a, space: s, parent: b, type: .folder, title: "A")])
    let space = store.space(id: s)!
    #expect(store.node(id: a)?.parent === space.pinned || store.node(id: b)?.parent === space.pinned)
    await bridge.flush()
    #expect(await engine.hasPendingChanges)
  }

  @Test func keepsFieldsItDoesntKnow() async throws {
    let (store, bridge, _) = make()
    let s = Self.uuid(), t = Self.uuid()
    var change = node(t, space: s, url: "https://x/")
    // A field from the future (number 99), on the node.
    var raw = try change.specifics!.compassSpaces.node.serializedBytes() as [UInt8]
    raw += [0x9a, 0x06, 0x02, 0x68, 0x69]  // field 99, length 2, "hi"
    change.specifics!.compassSpaces.node = try SyncPb_CompassNode(serializedBytes: raw)
    bridge.applyRemote([space(s, "W"), change])
    store.rename(store.node(id: t)!, customTitle: "Mine")
    let out = try bridge.mirror[t]!.node.serializedBytes() as [UInt8]
    #expect(out.suffix(5) == [0x9a, 0x06, 0x02, 0x68, 0x69] || Data(out).range(of: Data([0x9a, 0x06, 0x02, 0x68, 0x69])) != nil)
    #expect(bridge.mirror[t]!.node.customTitle == "Mine")
  }

  @Test func throttlesTabChurn() {
    let (store, bridge, _) = make()
    var clock = Date(timeIntervalSince1970: 1_000_000)
    bridge.now = { clock }
    let space = store.addSpace(name: "W")
    let tab = store.addTab(url: "https://a/", to: space.today, now: clock)!
    store.update(tab, url: "https://a/1")
    #expect(bridge.mirror[tab.id]?.node.url == "https://a/")  // held
    #expect(bridge.throttled[tab.id] != nil)
    clock.addTimeInterval(11)
    store.update(tab, url: "https://a/2")
    #expect(bridge.mirror[tab.id]?.node.url == "https://a/2")  // 10 s passed
    store.rename(tab, customTitle: "T")  // structural: at once
    #expect(bridge.mirror[tab.id]?.node.customTitle == "T")
  }

  @Test func remoteDeletionRemovesAndRescuesChildren() {
    let (store, bridge, _) = make()
    let s = Self.uuid(), f = Self.uuid(), c = Self.uuid()
    bridge.applyRemote([
      space(s, "W"), node(f, space: s, type: .folder), node(c, space: s, parent: f, url: "https://c/"),
    ])
    bridge.applyRemote([
      RemoteChange(
        clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: f), specifics: nil, serverID: f, version: 2,
        mtime: 0, ctime: 0, originatorCacheGUID: "mac")
    ])
    #expect(store.node(id: f) == nil)
    #expect(store.node(id: c)?.parent === store.space(id: s)!.pinned)
  }

  @Test func theShownEntrySurvivesARemoteClose() async {
    let (store, bridge, engine) = make()
    let s = Self.uuid(), t = Self.uuid(), other = Self.uuid()
    bridge.applyRemote([
      space(s, "W"), node(t, space: s, section: .today, url: "https://shown/"),
      node(other, space: s, section: .today, url: "https://other/"),
    ])
    var replaced: [(String, String)] = []
    bridge.isShown = { $0 == t }
    bridge.onReplaced = { replaced.append(($0, $1)) }
    func deletion(_ id: String) -> RemoteChange {
      RemoteChange(
        clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: id), specifics: nil, serverID: id, version: 2,
        mtime: 0, ctime: 0, originatorCacheGUID: "mac")
    }
    bridge.applyRemote([deletion(t), deletion(other)])
    let today = store.space(id: s)!.today
    #expect(today.children.map(\.url) == ["https://shown/"])
    #expect(today.children[0].id != t)
    #expect(replaced.first?.0 == t && replaced.first?.1 == today.children[0].id)
    await bridge.flush()
    #expect(await engine.hasPendingChanges)  // the new entry goes to the chain
  }

  @Test func initialMergeFoldsAnUntouchedHome() {
    let store = SpaceStore()
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let bridge = CompassSpacesBridge(store: store, engine: engine, mirrorURL: nil)
    let home = store.addSpace(name: "Home")
    store.addTab(url: "https://phone/", to: home.today)
    store.addTab(url: "https://shared/", to: store.favorites)
    let s = Self.uuid(), fav = Self.uuid()
    bridge.applyRemote([space(s, "Work"), node(fav, space: "", section: .favorites, url: "https://shared/")])
    let local = bridge.mergeWithChain()
    #expect(store.spaces.map(\.name) == ["Work"])
    #expect(store.space(id: s)!.today.children.map(\.url) == ["https://phone/"])
    #expect(store.favorites.children.count == 1)
    #expect(local.count == 1)  // the phone's Today tab
  }
}
