// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
@testable import CompassSync
import Foundation
import SyncProto
import Testing

/// Live folders over COMPASS_SPACES (docs/design/live-folders.md, "Sync"):
/// the folder's source syncs, its entries never do.
@MainActor
struct LiveFolderSyncTests {
  let helper = CompassSpacesBridgeTests()

  func items(_ keys: String...) -> [LiveItem] {
    keys.map { LiveItem(key: $0, url: "https://example.com/\($0)", title: $0) }
  }

  func liveNode(_ id: String, space: String, plugin: String = "compass.rss", config: String = "{}") -> RemoteChange {
    var change = helper.node(id, space: space, type: .folder, title: "News")
    var live = SyncPb_CompassLiveSource()
    live.pluginID = plugin
    live.configJson = config
    change.specifics!.compassSpaces.node.liveSource = live
    return change
  }

  @Test func theSourceSyncsAndTheEntriesDont() async {
    let (store, bridge, engine) = helper.make()
    let space = store.addSpace(name: "Work")
    let source = LiveSource(pluginID: "compass.rss", configJSON: #"{"url":"https://example.com/feed"}"#)
    let folder = store.addLiveFolder(name: "News", source: source, to: space.pinned)!
    store.replaceLiveItems(folder, items: items("a", "b"))
    let live = bridge.mirror[folder.id]!.node.liveSource
    #expect(live.pluginID == "compass.rss" && live.configJson == source.configJSON && !live.hasPluginURL)
    #expect(bridge.mirror[folder.id]!.node.type == NodeType.folder.rawValue)
    for entry in folder.children { #expect(bridge.mirror[entry.id] == nil && bridge.desired(entry) == nil) }
    // Nor does the initial merge send them.
    let merged = bridge.mergeWithChain().map(\.tag)
    #expect(!merged.contains { tag in folder.children.contains { $0.id == tag } })

    // An entry moved out is an ordinary tab, and syncs.
    let a = folder.children[0]
    store.move(a, to: space.pinned, at: 0)
    #expect(bridge.mirror[a.id] != nil)
    await bridge.flush()
    #expect(await engine.knownTags(.compassSpaces).contains(a.id))
  }

  @Test func remoteLiveFolderAndItsChanges() {
    let (store, bridge, _) = helper.make()
    let s = CompassSpacesBridgeTests.uuid(), f = CompassSpacesBridgeTests.uuid()
    bridge.applyRemote([helper.space(s, "W"), liveNode(f, space: s, config: #"{"url":"https://a.example/"}"#)])
    let folder = store.node(id: f)!
    #expect(folder.liveSource?.pluginID == "compass.rss")
    store.replaceLiveItems(folder, items: items("a", "b"))

    // A change from elsewhere that doesn't mention the entries keeps them.
    bridge.applyRemote([liveNode(f, space: s, config: #"{"url":"https://b.example/"}"#)])
    #expect(folder.liveSource?.configJSON == #"{"url":"https://b.example/"}"#)
    #expect(folder.children.count == 2)

    // Made a normal folder elsewhere: this device's entries go.
    bridge.applyRemote([helper.node(f, space: s, type: .folder, title: "News")])
    #expect(!folder.isLiveFolder && folder.children.isEmpty)
  }

  @Test func remoteDeleteTakesTheEntries() {
    let (store, bridge, _) = helper.make()
    let s = CompassSpacesBridgeTests.uuid(), f = CompassSpacesBridgeTests.uuid()
    bridge.applyRemote([helper.space(s, "W"), liveNode(f, space: s)])
    let folder = store.node(id: f)!
    store.replaceLiveItems(folder, items: items("a"))
    let entry = folder.children[0]
    bridge.applyRemote([
      RemoteChange(
        clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: f), specifics: nil, serverID: f, version: 2,
        mtime: 0, ctime: 0, originatorCacheGUID: "mac")
    ])
    #expect(store.node(id: f) == nil && store.node(id: entry.id) == nil)
  }

  @Test func conversionClearsTheSourceButAPlainFolderKeepsIt() {
    let (store, bridge, _) = helper.make()
    let s = CompassSpacesBridgeTests.uuid(), f = CompassSpacesBridgeTests.uuid()
    bridge.applyRemote([helper.space(s, "W"), liveNode(f, space: s)])
    let folder = store.node(id: f)!
    store.replaceLiveItems(folder, items: items("a"))
    store.convertToPlainFolder(folder)
    #expect(!bridge.mirror[f]!.node.hasLiveSource)
    #expect(bridge.mirror[folder.children[0].id] != nil)  // the entry syncs now

    // A live folder in Today (where none can be) stays plain here, and its
    // source isn't cleared on the server when this device changes it.
    let t = CompassSpacesBridgeTests.uuid()
    var today = liveNode(t, space: s)
    today.specifics!.compassSpaces.node.section = Section.today.rawValue
    bridge.applyRemote([today])
    let plain = store.node(id: t)!
    #expect(!plain.isLiveFolder)
    store.rename(plain, customTitle: "Renamed")
    #expect(bridge.mirror[t]!.node.hasLiveSource && bridge.mirror[t]!.node.title == "Renamed")
  }
}
