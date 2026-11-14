// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
@testable import CompassSync
import Foundation
import SwiftProtobuf
import SyncProto
import Testing

/// A folder's NameSource on the wire (CompassNode.name_source, 16), as the
/// desktop's CompassSpacesSyncBridgeTest: 1 OPENER, 2 CLAUDE, unset the
/// user's; a missing field (an older client) reads as the user's, and a
/// value this client doesn't know is passed on.
@MainActor
struct NameSourceSyncTests {
  let base = CompassSpacesBridgeTests()

  func folder(_ id: String, space: String, title: String, source: Int32?) -> RemoteChange {
    var change = base.node(id, space: space, section: .today, type: .folder, title: title)
    if let source { change.specifics!.compassSpaces.node.nameSource = source }
    return change
  }

  @Test func sendsWhereANameCameFrom() {
    let (store, bridge, _) = base.make()
    let space = store.addSpace(name: "Work")
    let folder = store.addFolder(name: "Tokio", to: space.today)!
    #expect(!bridge.desired(folder)!.node.hasNameSource)
    store.markLinkFolder(folder)
    #expect(bridge.mirror[folder.id]?.node.nameSource == 1)
    store.setFolderName(folder, "Rust Async", source: .claude)
    #expect(bridge.mirror[folder.id]?.node.nameSource == 2)
    #expect(bridge.mirror[folder.id]?.node.title == "Rust Async")
    // A rename by hand: the user's, which is no field at all.
    store.rename(folder, customTitle: "Mine")
    #expect(bridge.mirror[folder.id]?.node.hasNameSource == false)
    // Tabs never carry one.
    let tab = store.addTab(url: "https://a.example/", to: folder)!
    #expect(!bridge.desired(tab)!.node.hasNameSource)
  }

  @Test func appliesWhereANameCameFrom() {
    let (store, bridge, _) = base.make()
    let s = CompassSpacesBridgeTests.uuid(), opener = CompassSpacesBridgeTests.uuid()
    let claude = CompassSpacesBridgeTests.uuid(), old = CompassSpacesBridgeTests.uuid()
    bridge.applyRemote([
      base.space(s, "Work"),
      folder(opener, space: s, title: "Tokio", source: 1),
      folder(claude, space: s, title: "Rust Async", source: 2),
      folder(old, space: s, title: "Old", source: nil),
    ])
    #expect(store.node(id: opener)?.nameSource == .opener)
    #expect(store.node(id: claude)?.nameSource == .claude)
    // Missing: the user's, as from a client that predates the field.
    #expect(store.node(id: old)?.nameSource == .user)
    // Not made here: Claude never names it on this device.
    #expect(store.node(id: opener)?.linkFolderHere == false)

    // Renamed by the user on another device: the user's here too.
    bridge.applyRemote([folder(claude, space: s, title: "Mine", source: nil)])
    #expect(store.node(id: claude)?.nameSource == .user && store.node(id: claude)?.title == "Mine")
    // Applying commits nothing back.
    #expect(bridge.mirror[claude]?.node.hasNameSource == false)
  }

  @Test func roundTripsThroughTheWire() throws {
    let (store, bridge, _) = base.make()
    let space = store.addSpace(name: "Work")
    let folder = store.addFolder(name: "Tokio", to: space.today)!
    store.markLinkFolder(folder)
    store.setFolderName(folder, "Rust Async", source: .claude)
    let bytes: [UInt8] = try bridge.mirror[folder.id]!.node.serializedBytes()
    let decoded = try SyncPb_CompassNode(serializedBytes: bytes)
    #expect(decoded.nameSource == 2)
    // Field 16, varint: tag 0x80 0x01, value 2.
    #expect(Data(bytes).range(of: Data([0x80, 0x01, 0x02])) != nil)

    let (other, otherBridge, _) = base.make()
    var change = base.node(folder.id, space: space.id, section: .today, type: .folder, title: "Rust Async")
    change.specifics!.compassSpaces.node = decoded
    otherBridge.applyRemote([base.space(space.id, "Work"), change])
    #expect(other.node(id: folder.id)?.nameSource == .claude)
  }

  @Test func passesOnAValueItDoesntKnow() {
    let (store, bridge, _) = base.make()
    let s = CompassSpacesBridgeTests.uuid(), f = CompassSpacesBridgeTests.uuid()
    bridge.applyRemote([base.space(s, "Work"), folder(f, space: s, title: "Future", source: 7)])
    let node = store.node(id: f)!
    #expect(node.nameSource == .user)
    store.setCollapsed(node, true)
    #expect(bridge.mirror[f]?.node.nameSource == 7)
  }
}
