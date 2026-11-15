// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
@testable import CompassSync
import Foundation
import SwiftProtobuf
import SyncProto
import Testing

/// Link rules on the wire (docs/design/link-routing.md, "Sync"): one
/// CompassLinkRule entity per domain, as the desktop's
/// CompassSpacesSyncBridgeTest.LinkRule*.
@MainActor
struct LinkRoutingSyncTests {
  private func bridge(_ store: SpaceStore) -> CompassSpacesBridge {
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let bridge = CompassSpacesBridge(store: store, engine: engine, mirrorURL: nil)
    _ = bridge.mergeWithChain()
    return bridge
  }

  private func change(_ s: SyncPb_CompassSpacesSpecifics, version: Int64 = 1) -> RemoteChange {
    let id = CompassSpacesBridge.uuid(of: s)!
    var e = SyncPb_EntitySpecifics()
    e.compassSpaces = s
    return RemoteChange(
      clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: id), specifics: e, serverID: id, version: version,
      mtime: 0, ctime: 0, originatorCacheGUID: "mac")
  }

  private func rule(_ domain: String, _ space: String?) -> SyncPb_CompassSpacesSpecifics {
    var s = SyncPb_CompassSpacesSpecifics()
    s.linkRule.uuid = CompassSpacesBridge.linkRuleUUID(domain: domain)
    s.linkRule.domain = domain
    if let space { s.linkRule.spaceUuid = space }
    return s
  }

  private func space(_ id: String, _ name: String) -> SyncPb_CompassSpacesSpecifics {
    var s = SyncPb_CompassSpacesSpecifics()
    s.space.uuid = id
    s.space.name = name
    return s
  }

  private func deletion(_ id: String) -> RemoteChange {
    RemoteChange(
      clientTagHash: ClientTagHash.make(type: .compassSpaces, tag: id), specifics: nil, serverID: id, version: 9,
      mtime: 0, ctime: 0, originatorCacheGUID: "mac")
  }

  @Test func keysAreTheDesktops() {
    // SHA-256("compass-link-rule:github.com"), as LinkRuleKey().
    #expect(CompassSpacesBridge.linkRuleUUID(domain: "github.com") == "f3a7f943-0067-54ca-859e-23416c800499")
    #expect(CompassSpacesBridge.linkRuleUUID(domain: "github.com") != CompassSpacesBridge.faviconUUID(host: "github.com"))
    #expect(CompassSpacesBridge.isValidUUID(CompassSpacesBridge.linkRuleUUID(domain: "example.com")))
  }

  @Test func theWireShape() throws {
    let s = rule("github.com", "0f6d3f4e-1111-4222-8333-444444444444")
    let bytes = [UInt8](try s.serializedData())
    // CompassSpacesSpecifics field 6 (length-delimited): tag 0x32.
    #expect(bytes.first == 0x32)
    let back = try SyncPb_CompassSpacesSpecifics(serializedBytes: bytes)
    #expect(back.linkRule.domain == "github.com" && back.linkRule.spaceUuid == "0f6d3f4e-1111-4222-8333-444444444444")
    #expect(back.linkRule.uuid == CompassSpacesBridge.linkRuleUUID(domain: "github.com"))
  }

  @Test func localRulesGoUpAndComeDown() async {
    let store = SpaceStore()
    let bridge = bridge(store)
    let work = store.addSpace(name: "Work")
    store.setLinkRule("github.com", to: work)
    let id = CompassSpacesBridge.linkRuleUUID(domain: "github.com")
    #expect(bridge.mirror[id]?.linkRule.spaceUuid == work.id)
    #expect(bridge.mirror[id]?.linkRule.domain == "github.com")
    await bridge.flush()
    #expect(await bridge.engine.hasPendingChanges)
    // Removed here: deleted on the server.
    store.removeLinkRule("github.com", profile: "")
    #expect(bridge.mirror[id] == nil)
  }

  @Test func remoteRulesApplyWithoutEcho() async {
    let store = SpaceStore()
    let bridge = bridge(store)
    let work = "0f6d3f4e-1111-4222-8333-444444444444"
    bridge.applyRemote([change(space(work, "Work")), change(rule("github.com", work))])
    #expect(store.linkRules == [LinkRule(domain: "github.com", spaceID: work)])
    #expect(store.linkSpace(for: URL(string: "https://gist.github.com/1")!)?.id == work)
    await bridge.flush()
    #expect(await bridge.engine.hasPendingChanges == false)
    // Changed, then deleted on another device.
    let home = "0f6d3f4e-1111-4222-8333-555555555555"
    bridge.applyRemote([change(space(home, "Home")), change(rule("github.com", home), version: 2)])
    #expect(store.linkRules == [LinkRule(domain: "github.com", spaceID: home)])
    bridge.applyRemote([deletion(CompassSpacesBridge.linkRuleUUID(domain: "github.com"))])
    #expect(store.linkRules.isEmpty)
    await bridge.flush()
    #expect(await bridge.engine.hasPendingChanges == false)
  }

  /// A rule whose space isn't here is kept and routes nothing; it routes
  /// once the space comes.
  @Test func aRuleOutlivesItsSpace() {
    let store = SpaceStore()
    let bridge = bridge(store)
    let later = "0f6d3f4e-1111-4222-8333-666666666666"
    bridge.applyRemote([change(rule("linear.app", later))])
    #expect(store.linkRules.count == 1)
    #expect(store.linkSpace(for: URL(string: "https://linear.app")!) == nil)
    bridge.applyRemote([change(space(later, "Later"))])
    #expect(store.linkSpace(for: URL(string: "https://linear.app")!)?.id == later)
  }

  /// A later version's form (no `space_uuid`) and invalid entities are
  /// ignored and never deleted; a rule set here for that domain fills it
  /// in and keeps what this version doesn't know.
  @Test func aMissingFieldIsIgnoredAndKept() async throws {
    let store = SpaceStore()
    let bridge = bridge(store)
    var future = rule("gitlab.com", nil)
    var bytes = try future.serializedData()
    future = try SyncPb_CompassSpacesSpecifics(serializedBytes: bytes)
    // An unknown field inside the rule: field 9, varint 1.
    var inner = try future.linkRule.serializedData()
    inner.append(contentsOf: [0x48, 0x01])
    future.linkRule = try SyncPb_CompassLinkRule(serializedBytes: inner)
    var bad = rule("GitHub.com", "0f6d3f4e-1111-4222-8333-444444444444")
    bad.linkRule.uuid = CompassSpacesBridge.linkRuleUUID(domain: "GitHub.com")
    var wrongKey = rule("example.com", "0f6d3f4e-1111-4222-8333-444444444444")
    wrongKey.linkRule.uuid = CompassSpacesBridge.linkRuleUUID(domain: "example.org")
    bridge.applyRemote([change(future), change(bad), change(wrongKey)])
    #expect(store.linkRules.isEmpty)
    let id = CompassSpacesBridge.linkRuleUUID(domain: "gitlab.com")
    #expect(bridge.mirror[id] != nil)
    await bridge.flush()
    #expect(await bridge.engine.hasPendingChanges == false)
    // Set here: the entity gets a space and keeps the unknown field.
    let work = store.addSpace(name: "Work")
    store.setLinkRule("gitlab.com", to: work)
    let desired = try #require(bridge.mirror[id]?.linkRule)
    #expect(desired.spaceUuid == work.id && desired.domain == "gitlab.com")
    bytes = try desired.serializedData()
    #expect(bytes.suffix(2) == Data([0x48, 0x01]))
  }

  /// A rule for a space that stays on this device isn't uploaded; turning
  /// the space's sync off deletes it on the others, on uploads it again.
  @Test func aLocalSpacesRuleStaysHere() {
    let store = SpaceStore()
    let bridge = bridge(store)
    let work = store.addSpace(name: "Work")
    let id = CompassSpacesBridge.linkRuleUUID(domain: "github.com")
    store.setLinkRule("github.com", to: work)
    #expect(bridge.mirror[id] != nil)
    store.setSynced(work, false)
    #expect(bridge.mirror[id] == nil)
    store.setLinkRule("example.com", to: work)
    #expect(bridge.mirror[CompassSpacesBridge.linkRuleUUID(domain: "example.com")] == nil)
    store.setSynced(work, true)
    #expect(bridge.mirror[id] != nil)
  }

  /// The initial merge sends this device's rules the chain doesn't have.
  @Test func theInitialMergeSendsLocalRules() {
    let store = SpaceStore()
    let work = store.addSpace(name: "Work")
    store.setLinkRule("github.com", to: work)
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let bridge = CompassSpacesBridge(store: store, engine: engine, mirrorURL: nil)
    let merged = bridge.mergeWithChain()
    let id = CompassSpacesBridge.linkRuleUUID(domain: "github.com")
    #expect(merged.contains { $0.tag == id && $0.specifics.compassSpaces.linkRule.spaceUuid == work.id })
  }

  /// Through the wire from one store to another.
  @Test func roundTripBetweenTwoStores() throws {
    let mac = SpaceStore()
    let macBridge = bridge(mac)
    let work = mac.addSpace(name: "Work")
    mac.setLinkRule("linear.app", to: work)
    let phone = SpaceStore()
    let phoneBridge = bridge(phone)
    let wireSpace = try SyncPb_CompassSpacesSpecifics(serializedBytes: try macBridge.mirror[work.id]!.serializedData())
    let wireRule = try SyncPb_CompassSpacesSpecifics(
      serializedBytes: try macBridge.mirror[CompassSpacesBridge.linkRuleUUID(domain: "linear.app")]!.serializedData())
    phoneBridge.applyRemote([change(wireSpace), change(wireRule)])
    #expect(phone.linkSpace(for: URL(string: "https://linear.app/team")!)?.id == work.id)
  }
}
