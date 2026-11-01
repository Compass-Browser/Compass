// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import SyncProto

/// The repo's Go sync server (server/ on sync-main), started per test on a
/// free port with its own SQLite file. Set COMPASS_SYNC_SERVER to the binary
/// (`compass ios test --sync-server` builds it); without it these tests skip.
final class TestServer: @unchecked Sendable {
  static let binary = ProcessInfo.processInfo.environment["COMPASS_SYNC_SERVER"]
  static var available: Bool { binary.map { FileManager.default.isExecutableFile(atPath: $0) } ?? false }

  let process = Process()
  let directory: URL
  let url: URL

  init() async throws {
    directory = FileManager.default.temporaryDirectory
      .appendingPathComponent("compass-sync-test-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let port = Int.random(in: 20000..<40000)
    url = URL(string: "http://127.0.0.1:\(port)/v2")!
    process.executableURL = URL(fileURLWithPath: Self.binary!)
    process.environment = [
      "COMPASS_SYNC_ADDR": "127.0.0.1:\(port)",
      "COMPASS_SYNC_DB": directory.appendingPathComponent("sync.db").path,
      "ENV": "local",
      "PATH": "/usr/bin:/bin",
    ]
    process.standardOutput = FileHandle(forWritingAtPath: "/dev/null")
    process.standardError = try FileHandle(
      forWritingTo: {
        let log = directory.appendingPathComponent("server.log")
        FileManager.default.createFile(atPath: log.path, contents: nil)
        return log
      }())
    try process.run()
    let health = URL(string: "http://127.0.0.1:\(port)/healthz")!
    for _ in 0..<100 {
      if let (_, response) = try? await URLSession.shared.data(from: health),
        (response as? HTTPURLResponse)?.statusCode == 200
      {
        return
      }
      try await Task.sleep(for: .milliseconds(50))
    }
    throw URLError(.cannotConnectToHost)
  }

  var log: String {
    (try? String(contentsOf: directory.appendingPathComponent("server.log"), encoding: .utf8)) ?? ""
  }

  deinit {
    process.terminate()
    process.waitUntilExit()
    try? FileManager.default.removeItem(at: directory)
  }
}

/// A COMPASS_SPACES bridge over a dictionary: space name by UUID.
final class SpacesMemoryBridge: DataTypeBridge, @unchecked Sendable {
  let dataType = DataType.compassSpaces
  private let lock = NSLock()
  private var spaces: [String: String] = [:]
  var local: [String: String] = [:]

  func clientTag(for specifics: SyncPb_EntitySpecifics) -> String {
    specifics.compassSpaces.space.uuid
  }

  func apply(_ changes: [RemoteChange]) async {
    lock.withLock {
      for change in changes {
        if let s = change.specifics {
          spaces[s.compassSpaces.space.uuid] = s.compassSpaces.space.name
        } else {
          spaces = spaces.filter {
            ClientTagHash.make(type: .compassSpaces, tag: $0.key) != change.clientTagHash
          }
        }
      }
    }
  }

  func localDataForInitialMerge() async -> [(tag: String, specifics: SyncPb_EntitySpecifics)] {
    local.map { ($0.key, Self.specifics(uuid: $0.key, name: $0.value)) }
  }

  var names: [String: String] { lock.withLock { spaces } }

  func set(_ uuid: String, _ name: String, engine: SyncEngine) async {
    lock.withLock { spaces[uuid] = name }
    await engine.put(.compassSpaces, tag: uuid, specifics: Self.specifics(uuid: uuid, name: name))
  }

  func remove(_ uuid: String, engine: SyncEngine) async {
    _ = lock.withLock { spaces.removeValue(forKey: uuid) }
    await engine.delete(.compassSpaces, tag: uuid)
  }

  static func specifics(uuid: String, name: String) -> SyncPb_EntitySpecifics {
    var s = SyncPb_EntitySpecifics()
    s.compassSpaces.space.uuid = uuid
    s.compassSpaces.space.name = name
    return s
  }
}

/// One device on a chain: an engine, the spaces bridge and DEVICE_INFO.
struct TestDevice {
  let engine: SyncEngine
  let spaces = SpacesMemoryBridge()
  let devices: DeviceInfoBridge

  init(server: URL, chain: Chain, name: String, first: Bool, local: [String: String] = [:]) async {
    let metadata = SyncMetadata()
    engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server, cacheGUID: metadata.cacheGUID, authKey: chain.authKey),
      passphrase: chain.passphrase, store: MemorySyncMetadataStore(metadata), createChainIfEmpty: first)
    devices = DeviceInfoBridge(local: .init(name: name, model: "iPhone", appVersion: "0.1"))
    spaces.local = local
    await engine.register(spaces)
    await engine.register(devices)
  }

  @discardableResult
  func sync() async throws -> SyncReport {
    var report = try await engine.sync()
    await devices.refreshLocal(engine: engine)
    if await engine.hasPendingChanges { report = try await engine.sync() }
    return report
  }
}
