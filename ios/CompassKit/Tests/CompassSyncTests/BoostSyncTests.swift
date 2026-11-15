// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import CompassModel
@testable import CompassSync
import Foundation
import SwiftProtobuf
import SyncProto
import Testing

/// Boosts on the wire (docs/design/boosts.md, "Sync"): one CompassBoost
/// entity per Boost, in the home account, the later edit winning.
@MainActor
struct BoostSyncTests {
  private func bridge(boosts: [Boost] = []) -> CompassSpacesBridge {
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let bridge = CompassSpacesBridge(store: SpaceStore(), engine: engine, mirrorURL: nil)
    bridge.localBoosts = { boosts }
    _ = bridge.mergeWithChain()
    return bridge
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

  private let sample = Boost(
    id: "6a1e2f3c-1111-4222-8333-444444444444", name: "Hide sidebar", match: "*.example.com/app", css: ".side{display:none}",
    js: "console.log(1)", jsWorld: .main, enabled: false, author: .claude, createTime: 13_400_000_000_000_000,
    updateTime: 13_400_000_000_000_100)

  @Test func theWireShape() throws {
    var s = SyncPb_CompassSpacesSpecifics()
    s.boost.id = sample.id
    s.boost.name = sample.name
    let bytes = [UInt8](try s.serializedData())
    // CompassSpacesSpecifics field 7 (length-delimited): tag 0x3a.
    #expect(bytes.first == 0x3a)
    let back = try SyncPb_CompassSpacesSpecifics(serializedBytes: bytes)
    #expect(back.boost.id == sample.id && back.boost.name == sample.name)
    #expect(CompassSpacesBridge.uuid(of: back) == sample.id)
  }

  @Test func protoRoundTrip() throws {
    let b = bridge()
    let specifics = b.desired(sample)
    let back = try SyncPb_CompassSpacesSpecifics(serializedBytes: try specifics.serializedData())
    #expect(CompassSpacesBridge.boost(back.boost) == sample)
  }

  @Test func theInitialMergeSendsLocalBoosts() {
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let bridge = CompassSpacesBridge(store: SpaceStore(), engine: engine, mirrorURL: nil)
    bridge.localBoosts = { [self] in [sample] }
    let merged = bridge.mergeWithChain()
    #expect(merged.contains { $0.tag == sample.id && $0.specifics.compassSpaces.boost.css == sample.css })
  }

  @Test func localChangesCommitAndDelete() async {
    let b = bridge()
    b.boostsChanged([sample])
    #expect(b.mirror[sample.id]?.boost.js == sample.js)
    await b.flush()
    #expect(await b.engine.hasPendingChanges)
    #expect(await b.engine.knownTags(.compassSpaces).contains(sample.id))
    // Gone here: deleted from the account (never committed, so it just
    // goes).
    b.boostsChanged([])
    #expect(b.mirror[sample.id] == nil)
    await b.flush()
    #expect(await !b.engine.knownTags(.compassSpaces).contains(sample.id))
  }

  @Test func remoteChangesArrive() {
    let b = bridge()
    var got: [Boost] = []
    var gone: Set<String> = []
    b.onBoosts = { put, deleted in
      got = put
      gone = deleted
    }
    var s = SyncPb_CompassSpacesSpecifics()
    s.boost = b.desired(sample).boost
    b.applyRemote([change(s)])
    #expect(got == [sample] && gone.isEmpty)
    b.applyRemote([deletion(sample.id)])
    #expect(gone == [sample.id])
  }

  /// A remote edit older than the local one loses (BoostList.applyRemote).
  @Test func laterEditWins() {
    var list = BoostList(boosts: [sample])
    var older = sample
    older.name = "older"
    older.updateTime = sample.updateTime - 1
    let ok1 = !list.applyRemote([older], deleted: [])
    #expect(ok1)
    var newer = sample
    newer.name = "newer"
    newer.updateTime = sample.updateTime + 1
    let ok2 = list.applyRemote([newer], deleted: [])
    #expect(ok2)
    #expect(list.find(sample.id)?.name == "newer")
  }
}
