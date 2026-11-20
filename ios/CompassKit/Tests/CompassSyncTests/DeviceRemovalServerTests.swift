// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import Testing

/// Removing a device against the real Go server: the entry goes from the
/// others' DEVICE_INFO, the removed device is refused everywhere, and it
/// comes back by joining again with a new cache GUID.
@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
struct DeviceRemovalServerTests {
  let chain = try! Chain(seed: Chain.generateSeed())

  @Test func removeAndJoinAgain() async throws {
    let server = try await TestServer()
    // Base64 GUIDs with every character that needs escaping.
    let macGUID = "mac+/mac+/mac+/mac+/A="
    let phoneGUID = "ph+/one+/ph+/one+/BB=="
    let mac = await TestDevice(server: server.url, chain: chain, name: "Mac", first: true, cacheGUID: macGUID)
    let phone = await TestDevice(server: server.url, chain: chain, name: "iPhone", first: false, cacheGUID: phoneGUID)
    try await mac.sync()
    try await phone.sync()
    try await mac.sync()
    #expect(Set(mac.devices.allDevices.map(\.cacheGUID)) == [macGUID, phoneGUID], "\(server.log)")

    let remover = DeviceRemover(serverURL: server.url, cacheGUID: macGUID, authKey: chain.authKey)
    // Not itself: that's leaving.
    await #expect(throws: DeviceRemover.Failure.thisDevice) { try await remover.remove(macGUID) }
    try await remover.remove(phoneGUID)
    // Again: done already.
    try await remover.remove(phoneGUID)

    // The Mac's next sync brings the tombstone.
    try await mac.sync()
    #expect(mac.devices.allDevices.map(\.name) == ["Mac"], "\(server.log)")

    // The phone is refused: its sync, its push registration, its stream,
    // and removing others.
    await #expect(throws: SyncEngineError.connection(.deviceRemoved)) { try await phone.sync() }
    await #expect(throws: SyncConnectionError.deviceRemoved) {
      try await Push.register(
        token: Data(repeating: 1, count: 32), environment: .sandbox, server: server.url, cacheGUID: phoneGUID,
        authKey: chain.authKey)
    }
    let phoneRemover = DeviceRemover(serverURL: server.url, cacheGUID: phoneGUID, authKey: chain.authKey)
    await #expect(throws: DeviceRemover.Failure.deviceRemoved) { try await phoneRemover.remove(macGUID) }
    let removed = Flag()
    let notifier = SyncNotifier(serverURL: server.url, cacheGUID: phoneGUID, authKey: chain.authKey)
    let listening = Task { await notifier.run(changed: { _ in }, removed: { await removed.raise() }) }
    _ = await listening.value
    #expect(await removed.set)

    // Leaving and joining again: a new cache GUID, and it syncs.
    let again = await TestDevice(server: server.url, chain: chain, name: "iPhone", first: false)
    let newGUID = await again.engine.cacheGUID
    #expect(newGUID != phoneGUID)
    await again.spaces.set("s", "Back", engine: again.engine)
    try await again.sync()
    try await mac.sync()
    #expect(mac.spaces.names["s"] == "Back")
    #expect(Set(mac.devices.allDevices.map(\.cacheGUID)) == [macGUID, newGUID])
  }

  /// The removed device's open stream closes, and the other devices (not
  /// the one that removed it) hear the DEVICE_INFO change.
  @Test func streamsHearTheRemoval() async throws {
    let server = try await TestServer()
    let mac = await TestDevice(server: server.url, chain: chain, name: "Mac", first: true)
    let phone = await TestDevice(server: server.url, chain: chain, name: "iPhone", first: false)
    try await mac.sync()
    try await phone.sync()
    let macGUID = await mac.engine.cacheGUID
    let phoneGUID = await phone.engine.cacheGUID

    let removed = Flag()
    let phoneStream = Task {
      await SyncNotifier(serverURL: server.url, cacheGUID: phoneGUID, authKey: chain.authKey)
        .run(changed: { _ in }, removed: { await removed.raise() })
    }
    let heard = Heard()
    let iPadStream = Task {
      await SyncNotifier(serverURL: server.url, cacheGUID: "ipad-guid", authKey: chain.authKey).run { await heard.add($0) }
    }
    defer { iPadStream.cancel() }
    for _ in 0..<100 where await heard.events.isEmpty { try await Task.sleep(for: .milliseconds(20)) }

    try await DeviceRemover(serverURL: server.url, cacheGUID: macGUID, authKey: chain.authKey).remove(phoneGUID)
    // Closed by the server; the reconnect is refused for good.
    let ended = Task {
      try await Task.sleep(for: .seconds(15))
      phoneStream.cancel()
    }
    _ = await phoneStream.value
    ended.cancel()
    #expect(await removed.set, "\(server.log)")
    for _ in 0..<100 where await !heard.events.contains(where: { $0?.contains(.deviceInfo) == true }) {
      try await Task.sleep(for: .milliseconds(20))
    }
    #expect(await heard.events.contains { $0?.contains(.deviceInfo) == true })
  }

  /// The phone's device list survives a relaunch: the engine downloads only
  /// what changed since, so the bridge keeps its entries; an app from before
  /// that (no file) downloads DEVICE_INFO again.
  @Test func deviceListSurvivesARelaunch() async throws {
    let server = try await TestServer()
    let mac = await TestDevice(server: server.url, chain: chain, name: "Mac", first: true)
    try await mac.sync()
    let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: folder) }
    let metadata = MemorySyncMetadataStore(SyncMetadata())
    let guid = metadata.load()!.cacheGUID
    func launch(_ file: String) async -> (SyncEngine, DeviceInfoBridge) {
      let engine = SyncEngine(
        connection: HTTPSyncConnection(serverURL: server.url, cacheGUID: guid, authKey: chain.authKey),
        passphrase: chain.passphrase, store: metadata)
      let devices = DeviceInfoBridge(
        local: .init(name: "iPhone", model: "iPhone", appVersion: "1"), cacheGUID: guid,
        stateURL: folder.appendingPathComponent(file))
      if devices.needsRefetch { await engine.refetch(.deviceInfo) }
      await engine.register(devices)
      return (engine, devices)
    }
    let (engine, devices) = await launch("devices.plist")
    try await engine.sync()
    await devices.refreshLocal(engine: engine)
    try await engine.sync()
    #expect(Set(devices.allDevices.map(\.name)) == ["Mac", "iPhone"])

    // Relaunched: known before any cycle, and still after one.
    let (engine2, devices2) = await launch("devices.plist")
    #expect(!devices2.needsRefetch)
    #expect(Set(devices2.allDevices.map(\.name)) == ["Mac", "iPhone"])
    #expect(devices2.allDevices.filter(\.isThisDevice).map(\.name) == ["iPhone"])
    try await engine2.sync()
    #expect(Set(devices2.allDevices.map(\.name)) == ["Mac", "iPhone"])

    // An app that didn't keep them: all of them again.
    let (engine3, devices3) = await launch("other.plist")
    #expect(devices3.needsRefetch)
    try await engine3.sync()
    #expect(Set(devices3.allDevices.map(\.name)) == ["Mac", "iPhone"], "\(server.log)")
  }
}
