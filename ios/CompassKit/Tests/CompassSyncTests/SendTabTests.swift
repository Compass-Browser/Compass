// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import SyncProto
import Testing

/// A SEND_TAB_TO_SELF entry, as the desktop's SendTabToSelfBridge writes it.
private func sentTab(
  _ guid: String, to target: String, url: String = "https://example.org/a", opened: Bool = false,
  sharedAgo: TimeInterval = 60
) -> SyncPb_EntitySpecifics {
  var e = SyncPb_SendTabToSelfSpecifics()
  e.guid = guid
  e.url = url
  e.title = "Example \(guid)"
  e.deviceName = "MacBook"
  e.targetDeviceSyncCacheGuid = target
  e.sharedTimeUsec = Int64((Date().timeIntervalSince1970 - sharedAgo) * 1_000_000)
  e.opened = opened
  var s = SyncPb_EntitySpecifics()
  s.sendTabToSelf = e
  return s
}

private func change(_ s: SyncPb_EntitySpecifics?, tag: String) -> RemoteChange {
  RemoteChange(
    clientTagHash: ClientTagHash.make(type: .sendTabToSelf, tag: tag), specifics: s, serverID: tag, version: 1,
    mtime: 0, ctime: 0, originatorCacheGUID: "mac")
}

/// Collects what a bridge reports (onReceived is called off the test's task).
private final class Received: @unchecked Sendable {
  private let lock = NSLock()
  private var tabs: [SentTab] = []
  func add(_ new: [SentTab]) { lock.withLock { tabs += new } }
  var all: [SentTab] { lock.withLock { tabs } }
}

private func offlineEngine() -> SyncEngine {
  let metadata = SyncMetadata()
  return SyncEngine(
    connection: HTTPSyncConnection(
      serverURL: URL(string: "http://127.0.0.1:9")!, cacheGUID: metadata.cacheGUID, authKey: Data(count: 32)),
    passphrase: "unused", store: MemorySyncMetadataStore(metadata), createChainIfEmpty: false)
}

struct SendTabBridgeTests {
  // Only fresh, unopened http(s) entries for this phone, each once, also
  // across launches (the shown GUIDs are saved).
  @Test func showsEachTabForThisPhoneOnce() async throws {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("sendtab-\(UUID().uuidString).json")
    defer { try? FileManager.default.removeItem(at: url) }
    let received = Received()
    let bridge = SendTabBridge(engine: offlineEngine(), cacheGUID: "phone", stateURL: url)
    bridge.onReceived = { received.add($0) }
    #expect(bridge.clientTag(for: sentTab("g1", to: "phone")) == "g1")
    await bridge.apply([
      change(sentTab("g1", to: "phone"), tag: "g1"),
      change(sentTab("other", to: "ipad"), tag: "other"),
      change(sentTab("opened", to: "phone", opened: true), tag: "opened"),
      change(sentTab("old", to: "phone", sharedAgo: 11 * 24 * 60 * 60), tag: "old"),
      change(sentTab("js", to: "phone", url: "javascript:alert(1)"), tag: "js"),
    ])
    #expect(received.all.map(\.guid) == ["g1"])
    #expect(received.all.first?.deviceName == "MacBook")
    #expect(received.all.first?.title == "Example g1")

    // Again (another device touched it): not shown twice.
    await bridge.apply([change(sentTab("g1", to: "phone"), tag: "g1")])
    #expect(received.all.count == 1)

    // A new launch remembers it.
    let again = Received()
    let relaunched = SendTabBridge(engine: offlineEngine(), cacheGUID: "phone", stateURL: url)
    relaunched.onReceived = { again.add($0) }
    await relaunched.apply([change(sentTab("g1", to: "phone"), tag: "g1"), change(sentTab("g2", to: "phone"), tag: "g2")])
    #expect(again.all.map(\.guid) == ["g2"])
  }
}

/// The Mac's side in the server test: what SEND_TAB_TO_SELF holds.
private final class EntryRecorder: DataTypeBridge, @unchecked Sendable {
  let dataType = DataType.sendTabToSelf
  private let lock = NSLock()
  private var byGUID: [String: SyncPb_SendTabToSelfSpecifics] = [:]
  func clientTag(for specifics: SyncPb_EntitySpecifics) -> String { specifics.sendTabToSelf.guid }
  func apply(_ changes: [RemoteChange]) async {
    lock.withLock {
      for change in changes { if let s = change.specifics { byGUID[s.sendTabToSelf.guid] = s.sendTabToSelf } }
    }
  }
  func entry(_ guid: String) -> SyncPb_SendTabToSelfSpecifics? { lock.withLock { byGUID[guid] } }
}

@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
struct SendTabServerTests {
  let chain = try! Chain(seed: Chain.generateSeed())

  // The Mac sends a tab to the phone (encrypted, as Chromium would); the
  // phone shows it, and opening it marks the entry opened for the Mac.
  @Test func macSendsPhoneOpens() async throws {
    let server = try await TestServer()
    func engine(first: Bool) -> (SyncEngine, String) {
      let metadata = SyncMetadata()
      return (SyncEngine(
        connection: HTTPSyncConnection(serverURL: server.url, cacheGUID: metadata.cacheGUID, authKey: chain.authKey),
        passphrase: chain.passphrase, store: MemorySyncMetadataStore(metadata), createChainIfEmpty: first),
        metadata.cacheGUID)
    }
    let (mac, _) = engine(first: true)
    let recorder = EntryRecorder()
    await mac.register(recorder)
    try await mac.sync()

    let (phone, phoneGUID) = engine(first: false)
    let bridge = SendTabBridge(engine: phone, cacheGUID: phoneGUID, stateURL: nil)
    let received = Received()
    bridge.onReceived = { received.add($0) }
    await phone.register(bridge)
    try await phone.sync()

    await mac.put(.sendTabToSelf, tag: "tab-1", specifics: sentTab("tab-1", to: phoneGUID))
    try await mac.sync()
    try await phone.sync()
    #expect(received.all.map(\.url) == ["https://example.org/a"], "\(server.log)")

    await bridge.markOpened("tab-1")
    try await phone.sync()
    try await mac.sync()
    #expect(recorder.entry("tab-1")?.opened == true, "\(server.log)")
    #expect(received.all.count == 1)
  }
}
