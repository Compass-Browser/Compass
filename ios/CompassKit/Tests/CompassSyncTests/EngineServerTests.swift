// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import Testing

/// Two Swift clients on one chain against the real Go server.
@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
struct EngineServerTests {
  let chain = try! Chain(seed: Chain.generateSeed())

  @Test func roundTrip() async throws {
    let server = try await TestServer()
    let a = await TestDevice(server: server.url, chain: chain, name: "Mac", first: true)
    let b = await TestDevice(server: server.url, chain: chain, name: "iPhone", first: false)

    try await a.sync()
    await a.spaces.set("space-1", "Work", engine: a.engine)
    try await a.sync()

    try await b.sync()
    #expect(b.spaces.names == ["space-1": "Work"], "\(server.log)")

    await b.spaces.set("space-1", "Office", engine: b.engine)
    await b.spaces.set("space-2", "Home", engine: b.engine)
    try await b.sync()
    try await a.sync()
    #expect(a.spaces.names == ["space-1": "Office", "space-2": "Home"])

    await a.spaces.remove("space-2", engine: a.engine)
    try await a.sync()
    try await b.sync()
    #expect(b.spaces.names == ["space-1": "Office"])

    // Both devices see both entries in DEVICE_INFO.
    try await a.sync()
    let names = Set(a.devices.allDevices.map(\.name))
    #expect(names == ["Mac", "iPhone"])
    #expect(a.devices.allDevices.filter(\.isThisDevice).map(\.name) == ["Mac"])
  }

  @Test func joinerNeedsAStartedChain() async throws {
    let server = try await TestServer()
    let b = await TestDevice(server: server.url, chain: chain, name: "iPhone", first: false)
    await #expect(throws: SyncEngineError.chainNotStarted) { try await b.sync() }
  }

  @Test func wrongPassphrase() async throws {
    let server = try await TestServer()
    let a = await TestDevice(server: server.url, chain: chain, name: "Mac", first: true)
    try await a.sync()
    // Same auth key (same server account), other passphrase.
    let engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server.url, cacheGUID: "x", authKey: chain.authKey),
      passphrase: "wrong", store: MemorySyncMetadataStore())
    await #expect(throws: SyncEngineError.crypto(.wrongPassphrase)) { try await engine.sync() }
  }

  @Test func remoteWinsAConflict() async throws {
    let server = try await TestServer()
    let a = await TestDevice(server: server.url, chain: chain, name: "Mac", first: true)
    let b = await TestDevice(server: server.url, chain: chain, name: "iPhone", first: false)
    await a.spaces.set("s", "One", engine: a.engine)
    try await a.sync()
    try await b.sync()

    await a.spaces.set("s", "From Mac", engine: a.engine)
    await b.spaces.set("s", "From iPhone", engine: b.engine)
    try await a.sync()
    let report = try await b.sync()
    #expect(report.conflicts >= 1)
    try await a.sync()
    #expect(b.spaces.names["s"] == "From Mac")
    #expect(a.spaces.names["s"] == "From Mac")
  }

  @Test func localEditBeatsRemoteDeletion() async throws {
    let server = try await TestServer()
    let a = await TestDevice(server: server.url, chain: chain, name: "Mac", first: true)
    let b = await TestDevice(server: server.url, chain: chain, name: "iPhone", first: false)
    await a.spaces.set("s", "One", engine: a.engine)
    try await a.sync()
    try await b.sync()

    await a.spaces.remove("s", engine: a.engine)
    await b.spaces.set("s", "Kept", engine: b.engine)
    try await a.sync()
    try await b.sync()
    try await a.sync()
    #expect(a.spaces.names["s"] == "Kept", "\(server.log)")
  }

  @Test func initialMergeUploadsLocalData() async throws {
    let server = try await TestServer()
    let a = await TestDevice(server: server.url, chain: chain, name: "Mac", first: true)
    await a.spaces.set("shared", "Shared", engine: a.engine)
    try await a.sync()

    let b = await TestDevice(
      server: server.url, chain: chain, name: "iPhone", first: false,
      local: ["shared": "Shared", "phone-only": "Phone"])
    try await b.sync()
    try await a.sync()
    #expect(a.spaces.names == ["shared": "Shared", "phone-only": "Phone"])
  }

  @Test func encryptedOnTheServer() async throws {
    let server = try await TestServer()
    let a = await TestDevice(server: server.url, chain: chain, name: "Mac", first: true)
    await a.spaces.set("s", "Top Secret Space", engine: a.engine)
    try await a.sync()
    let db = try Data(contentsOf: server.directory.appendingPathComponent("sync.db"))
    let wal = (try? Data(contentsOf: server.directory.appendingPathComponent("sync.db-wal"))) ?? Data()
    #expect((db + wal).range(of: Data("Top Secret Space".utf8)) == nil)
  }
}
