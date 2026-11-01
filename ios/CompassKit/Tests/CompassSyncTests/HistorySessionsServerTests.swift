// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
@testable import CompassSync
import Foundation
import Testing

@MainActor
struct HistoryDevice {
  let engine: SyncEngine
  let history: HistoryStore
  let visits: HistoryBridge
  let directives: HistoryDeleteDirectivesBridge
  let sessions: SessionsBridge

  init(server: URL, chain: Chain, name: String, first: Bool) async throws {
    let metadata = SyncMetadata()
    engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server, cacheGUID: metadata.cacheGUID, authKey: chain.authKey),
      passphrase: chain.passphrase, store: MemorySyncMetadataStore(metadata), createChainIfEmpty: first)
    history = try HistoryStore()
    visits = HistoryBridge(history: history, engine: engine)
    directives = HistoryDeleteDirectivesBridge(history: history, engine: engine)
    sessions = SessionsBridge(engine: engine, deviceName: name, stateURL: nil)
    await engine.register(visits)
    await engine.register(directives)
    await engine.register(sessions)
  }

  func sync() async throws { try await engine.sync() }
}

@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
@MainActor
struct HistorySessionsServerTests {
  let chain = try! Chain(seed: Chain.generateSeed())

  @Test func historyAndDeleteDirectives() async throws {
    let server = try await TestServer()
    let mac = try await HistoryDevice(server: server.url, chain: chain, name: "Mac", first: true)
    try await mac.sync()
    let phone = try await HistoryDevice(server: server.url, chain: chain, name: "iPhone", first: false)
    try await phone.sync()

    let now = Date()
    let t = await mac.visits.record(url: "https://swift.org/", title: "Swift", at: now.addingTimeInterval(-60))!
    await mac.visits.record(url: "https://news.ycombinator.com/", title: "Hacker News", at: now)
    await phone.visits.record(url: "https://apple.com/", title: "Apple", at: now)
    // A later title for the same visit is an update of the same entity.
    await mac.visits.record(url: "https://swift.org/", title: "Swift.org", time: t)
    try await mac.sync()
    try await phone.sync()
    try await mac.sync()
    #expect(phone.history.count == 3, "\(server.log)")
    #expect(phone.history.search("swift").first?.title == "Swift.org")
    #expect(mac.history.search("apple").count == 1)
    // The engine forgets history entities once committed.
    #expect(await mac.engine.knownTags(.history).isEmpty)

    await phone.directives.deleteHistory(from: now.addingTimeInterval(-3600), to: now.addingTimeInterval(1))
    #expect(phone.history.count == 0)
    try await phone.sync()
    try await mac.sync()
    #expect(mac.history.count == 0)
  }

  @Test func sessionsBothWays() async throws {
    let server = try await TestServer()
    let mac = try await HistoryDevice(server: server.url, chain: chain, name: "MacBook", first: true)
    try await mac.sync()
    let phone = try await HistoryDevice(server: server.url, chain: chain, name: "iPhone", first: false)

    await mac.sessions.publish([
      .init(id: "a", url: "https://a.example/", title: "A", lastActive: Date()),
      .init(id: "b", url: "https://b.example/", title: "B", lastActive: Date()),
    ])
    await phone.sessions.publish([.init(id: "p", url: "https://phone.example/", title: "P", lastActive: Date())])
    try await mac.sync()
    try await phone.sync()
    try await mac.sync()

    let seenOnPhone = phone.sessions.foreignSessions()
    #expect(seenOnPhone.map(\.name) == ["MacBook"])
    #expect(seenOnPhone.first?.tabs.map(\.url) == ["https://a.example/", "https://b.example/"])
    #expect(mac.sessions.foreignSessions().first?.tabs.map(\.title) == ["P"])

    // Closing a tab on the Mac removes it on the phone.
    await mac.sessions.publish([.init(id: "b", url: "https://b.example/", title: "B", lastActive: Date())])
    try await mac.sync()
    try await phone.sync()
    #expect(phone.sessions.foreignSessions().first?.tabs.map(\.url) == ["https://b.example/"])
    // Republishing the same tabs commits nothing.
    await mac.sessions.publish([.init(id: "b", url: "https://b.example/", title: "B", lastActive: Date(timeIntervalSince1970: 0))])
    await mac.sessions.publish([.init(id: "b", url: "https://b.example/", title: "B", lastActive: Date(timeIntervalSince1970: 0))])
    try await mac.sync()
    #expect(await mac.engine.hasPendingChanges == false)
  }
}
