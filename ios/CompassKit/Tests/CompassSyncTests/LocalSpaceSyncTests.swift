// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
@testable import CompassSync
import Foundation
import SyncProto
import Testing

/// Spaces that stay on this device (docs/design/sync.md, "Spaces that don't
/// sync"), as the desktop's CompassSpacesSyncBridgeTest.LocalSpace* tests.
@MainActor
struct LocalSpaceSyncTests {
  let helper = CompassSpacesBridgeTests()

  @Test func aLocalSpaceIsNotUploaded() async {
    let (store, bridge, engine) = helper.make()
    let space = store.addSpace(name: "Private")
    store.setSynced(space, false)
    let tab = store.addTab(url: "https://bank.example/", title: "Bank", to: space.pinned)!
    #expect(bridge.mirror[space.id] == nil && bridge.mirror[tab.id] == nil)
    #expect(bridge.desired(tab) == nil)
    store.renameSpace(space, to: "Secret")
    #expect(bridge.mirror[space.id] == nil)
    // Nor does the initial merge send it.
    let merged = bridge.mergeWithChain().map(\.tag)
    #expect(!merged.contains(space.id) && !merged.contains(tab.id))
    await bridge.flush()
    let tags = await engine.knownTags(.compassSpaces)
    #expect(!tags.contains(space.id) && !tags.contains(tab.id))
  }

  @Test func turningSyncOffDeletesAndOnUploads() async {
    let (store, bridge, engine) = helper.make()
    let space = store.addSpace(name: "Work")
    let tab = store.addTab(url: "https://work.example/", title: "Work", to: space.pinned)!
    #expect(bridge.mirror[space.id] != nil && bridge.mirror[tab.id] != nil)
    await bridge.flush()

    store.setSynced(space, false)
    #expect(bridge.mirror[space.id] == nil && bridge.mirror[tab.id] == nil)
    await bridge.flush()
    var tags = await engine.knownTags(.compassSpaces)
    #expect(!tags.contains(space.id) && !tags.contains(tab.id))
    // Still here.
    #expect(store.space(id: space.id) != nil && store.node(id: tab.id) != nil)

    store.setSynced(space, true)
    #expect(bridge.mirror[space.id] != nil && bridge.mirror[tab.id] != nil)
    await bridge.flush()
    tags = await engine.knownTags(.compassSpaces)
    #expect(tags.contains(space.id) && tags.contains(tab.id))
  }

  /// A device that hadn't heard yet: its changes don't reach the local space,
  /// and are deleted again.
  @Test func remoteChangesDontTouchALocalSpace() {
    let (store, bridge, _) = helper.make()
    let space = store.addSpace(name: "Mine")
    let tab = store.addTab(url: "https://a.example/", title: "A", to: space.pinned)!
    store.setSynced(space, false)

    let added = CompassSpacesBridgeTests.uuid()
    bridge.applyRemote([
      helper.space(space.id, "Renamed"),
      helper.node(added, space: space.id, url: "https://b.example/", title: "B"),
      RemoteChange(
        clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: tab.id), specifics: nil, serverID: tab.id,
        version: 2, mtime: 0, ctime: 0, originatorCacheGUID: "mac"),
    ])
    #expect(space.name == "Mine")
    #expect(store.node(id: added) == nil)
    #expect(store.node(id: tab.id) != nil)
    #expect(bridge.mirror[space.id] == nil && bridge.mirror[added] == nil)
  }

  @Test func movingAnEntryIntoALocalSpaceDeletesIt() {
    let (store, bridge, _) = helper.make()
    let synced = store.addSpace(name: "Shared")
    let local = store.addSpace(name: "Local")
    store.setSynced(local, false)
    let tab = store.addTab(url: "https://a.example/", title: "A", to: synced.pinned)!
    #expect(bridge.mirror[tab.id] != nil)
    _ = store.move(tab, toSpace: local, section: .pinned)
    #expect(bridge.mirror[tab.id] == nil)
  }
}
