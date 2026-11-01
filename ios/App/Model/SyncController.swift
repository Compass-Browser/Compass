// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
import CompassSync
import Foundation
import Observation
import UIKit

/// Sync for the app: the chain in the Keychain, the engine and its bridges,
/// and when to run a cycle (launch, foreground, after local changes, the
/// server's poll interval). docs/design/ios.md, "Sync client".
@MainActor
@Observable
final class SyncController {
  enum State: Equatable {
    case off
    case syncing
    case on
    case failed(String)
  }

  static let defaultServer = URL(string: "https://sync.example.org/v2")!

  private(set) var state: State = .off
  private(set) var serverURL: URL?
  private(set) var devices: [SyncDevice] = []
  private(set) var lastSync: Date?
  private(set) var chain: Chain?
  var isOn: Bool { chain != nil }

  private let store: SpaceStore
  let history: HistoryStore
  private(set) var foreignSessions: [ForeignSession] = []
  private var visits: HistoryBridge?
  private var directives: HistoryDeleteDirectivesBridge?
  private var sessions: SessionsBridge?
  private let directory: URL
  private var engine: SyncEngine?
  private var spaces: CompassSpacesBridge?
  private var deviceInfo: DeviceInfoBridge?
  private var scheduled: Task<Void, Never>?
  /// Whether an entry is on screen (it survives a close elsewhere).
  var isShown: ((String) -> Bool)?
  /// A kept entry's new ID.
  var onReplaced: ((String, String) -> Void)?
  private var running = false
  private var again = false

  init(store: SpaceStore, history: HistoryStore, directory: URL) {
    self.store = store
    self.history = history
    self.directory = directory
    if let seed = Keychain.data("seed"), let server = Keychain.data("server").flatMap({ URL(dataRepresentation: $0, relativeTo: nil) }),
      let chain = try? Chain(seed: seed)
    {
      start(chain: chain, server: server, first: false)
    }
  }

  // MARK: Joining and leaving

  /// Joins with the code a Mac shows under "Add a device".
  func join(code: String, server: URL, waiting: @escaping @Sendable () -> Void) async -> PairingJoiner.Result {
    let joiner = PairingJoiner(
      transport: NetworkPairingTransport(serverURL: server), host: server.host()?.lowercased() ?? "",
      deviceName: UIDevice.current.name)
    let result = await joiner.join(code: code, waiting: waiting)
    if case .joined(let seed) = result, let chain = try? Chain(seed: seed) {
      save(chain: chain, server: server)
      start(chain: chain, server: server, first: false)
    }
    return result
  }

  func join(recoveryCode: String, server: URL) throws {
    let chain = try Chain(recoveryCode: recoveryCode)
    save(chain: chain, server: server)
    start(chain: chain, server: server, first: false)
  }

  /// Starts a new chain from this phone (no Mac syncing yet).
  func startNewChain(server: URL) -> Chain {
    let chain = try! Chain(seed: Chain.generateSeed())
    save(chain: chain, server: server)
    start(chain: chain, server: server, first: true)
    return chain
  }

  /// Stops syncing; local data stays.
  func leave() {
    scheduled?.cancel()
    Keychain.set(nil, "seed")
    Keychain.set(nil, "server")
    try? FileManager.default.removeItem(at: metadataURL)
    try? FileManager.default.removeItem(at: mirrorURL)
    engine = nil
    spaces = nil
    deviceInfo = nil
    chain = nil
    serverURL = nil
    devices = []
    foreignSessions = []
    visits = nil
    directives = nil
    sessions = nil
    state = .off
    store.onLocalChange = nil
  }

  private func save(chain: Chain, server: URL) {
    Keychain.set(chain.seed, "seed")
    Keychain.set(server.dataRepresentation, "server")
  }

  private var metadataURL: URL { directory.appendingPathComponent("sync-metadata.plist") }
  private var mirrorURL: URL { directory.appendingPathComponent("sync-spaces-mirror.json") }

  private func start(chain: Chain, server: URL, first: Bool) {
    self.chain = chain
    serverURL = server
    let metadataStore = FileSyncMetadataStore(url: metadataURL)
    let cacheGUID = metadataStore.load()?.cacheGUID ?? SyncMetadata().cacheGUID
    if metadataStore.load() == nil { metadataStore.save(SyncMetadata(cacheGUID: cacheGUID)) }
    let engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server, cacheGUID: cacheGUID, authKey: chain.authKey),
      passphrase: chain.passphrase, store: metadataStore, createChainIfEmpty: first)
    let spaces = CompassSpacesBridge(store: store, engine: engine, mirrorURL: mirrorURL)
    let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    let visits = HistoryBridge(history: history, engine: engine)
    let directives = HistoryDeleteDirectivesBridge(history: history, engine: engine)
    let sessions = SessionsBridge(
      engine: engine, deviceName: UIDevice.current.name,
      stateURL: directory.appendingPathComponent("sync-sessions.json"))
    sessions.onChange = { [weak self] list in
      Task { @MainActor in self?.foreignSessions = list }
    }
    self.visits = visits
    self.directives = directives
    self.sessions = sessions
    let deviceInfo = DeviceInfoBridge(
      local: .init(name: UIDevice.current.name, model: UIDevice.current.model, appVersion: version))
    deviceInfo.onChange = { [weak self] devices in
      Task { @MainActor in self?.devices = devices }
    }
    spaces.onLocalCommit = { [weak self] in self?.syncSoon(after: 2) }
    spaces.isShown = { [weak self] id in self?.isShown?(id) ?? false }
    spaces.onReplaced = { [weak self] old, new in self?.onReplaced?(old, new) }
    self.engine = engine
    self.spaces = spaces
    self.deviceInfo = deviceInfo
    Task {
      await engine.register(spaces)
      await engine.register(deviceInfo)
      await engine.register(visits)
      await engine.register(directives)
      await engine.register(sessions)
      syncSoon(after: 0)
    }
  }

  // MARK: Cycles

  /// Runs a cycle after `delay` seconds (coalesced).
  func syncSoon(after delay: TimeInterval = 0) {
    guard engine != nil else { return }
    scheduled?.cancel()
    scheduled = Task { [weak self] in
      if delay > 0 { try? await Task.sleep(for: .seconds(delay)) }
      guard !Task.isCancelled else { return }
      await self?.syncNow()
    }
  }

  func syncNow() async {
    guard let engine, let deviceInfo, let spaces else { return }
    if running {
      again = true
      return
    }
    running = true
    state = .syncing
    defer { running = false }
    do {
      await spaces.flush()
      try await engine.sync()
      await deviceInfo.refreshLocal(engine: engine)
      await sessions?.publish(openTabs())
      await spaces.flush()
      if await engine.hasPendingChanges { try await engine.sync() }
      lastSync = Date()
      state = .on
      devices = deviceInfo.allDevices
      foreignSessions = sessions?.foreignSessions() ?? []
    } catch SyncEngineError.chainNotStarted {
      state = .failed("No device has started sync on this chain yet.")
    } catch SyncEngineError.crypto {
      state = .failed("This chain was reset on another device. Join it again.")
    } catch SyncEngineError.connection(let error) {
      state = .failed("Can't reach the sync server (\(error)).")
    } catch {
      state = .failed("\(error)")
    }
    // The server's poll interval while the app is open (push comes in M7).
    let poll = await engine.pollInterval ?? 30
    if again {
      again = false
      syncSoon(after: 0)
    } else {
      syncSoon(after: max(poll, 15))
    }
  }

  var recoveryWords: [String] { chain?.recoveryWords ?? [] }

  /// This phone's open entries for SESSIONS: every space's Today tabs.
  private func openTabs() -> [SessionsBridge.LocalTab] {
    store.spaces.flatMap { $0.today.children.flatMap(\.tabs) }
      .filter { !$0.url.isEmpty }
      .sorted { ($0.lastActive ?? .distantPast) > ($1.lastActive ?? .distantPast) }
      .map { .init(id: $0.id, url: $0.url, title: $0.displayTitle, lastActive: $0.lastActive ?? Date()) }
  }

  // MARK: History

  /// A page finished loading in a tab: a visit (synced when sync is on).
  func recordVisit(url: String, title: String) {
    if let visits {
      Task { await visits.record(url: url, title: title) }
      syncSoon(after: 5)
    } else {
      history.add(Visit(time: Visit.time(Date()), url: url, title: title, originator: "local"))
    }
  }

  func clearHistory() {
    if let directives {
      Task {
        await directives.deleteHistory(from: Date(timeIntervalSince1970: 0), to: Date())
        syncSoon()
      }
    } else {
      history.deleteAll()
    }
  }
}
