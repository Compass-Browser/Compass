// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
import CompassSync
import Foundation
import Observation
import UIKit
import os

/// Sync for the app: the chain in the Keychain, one account per profile of
/// the chain (each with its engine and bridges), and when to run a cycle
/// (launch, foreground, after local changes, the server's poll interval).
/// docs/design/ios.md, "Sync client" and "M8".
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

  /// One profile's server account: the home profile's is the chain's own
  /// ("" here); every other profile has its own (sync.md, "Slice 5 notes").
  final class Account {
    let profile: String
    let engine: SyncEngine
    let spaces: CompassSpacesBridge
    let deviceInfo: DeviceInfoBridge
    let visits: HistoryBridge
    let directives: HistoryDeleteDirectivesBridge
    let sessions: SessionsBridge
    let passwords: PasswordsBridge
    /// The desktop's extensions, download only (docs/design/ios.md, "Web extensions").
    let extensions: ExtensionsBridge
    /// Tabs other devices send to this phone (docs/design/send-tab.md).
    let sentTabs: SendTabBridge
    let notifier: SyncNotifier
    var listening: Task<Void, Never>?
    var foreignSessions: [ForeignSession] = []

    init(
      profile: String, engine: SyncEngine, spaces: CompassSpacesBridge, deviceInfo: DeviceInfoBridge,
      visits: HistoryBridge, directives: HistoryDeleteDirectivesBridge, sessions: SessionsBridge,
      passwords: PasswordsBridge, extensions: ExtensionsBridge, sentTabs: SendTabBridge, notifier: SyncNotifier
    ) {
      self.profile = profile
      self.engine = engine
      self.spaces = spaces
      self.deviceInfo = deviceInfo
      self.visits = visits
      self.directives = directives
      self.sessions = sessions
      self.passwords = passwords
      self.extensions = extensions
      self.sentTabs = sentTabs
      self.notifier = notifier
    }
  }

  private(set) var state: State = .off
  private(set) var serverURL: URL?
  private(set) var devices: [SyncDevice] = []
  private(set) var lastSync: Date?
  private(set) var chain: Chain?
  /// The chain's profiles (home first), from the home account or pairing.
  private(set) var profiles: [ChainProfile] = []
  /// Other devices' open tabs, per account ("" = the home's).
  private(set) var sessionsByAccount: [String: [ForeignSession]] = [:]
  /// Extensions on the chain's other devices (every profile's account),
  /// for Settings → Extensions to offer.
  private(set) var syncedExtensions: [SyncedExtension] = []
  var isOn: Bool { chain != nil }

  private let store: SpaceStore
  let history: HistoryStore
  private let directory: URL
  private var spaceDirectory: SpaceDirectory?
  private var accounts: [String: Account] = [:]
  private var scheduled: Task<Void, Never>?
  private var active = true
  /// Whether an entry is on screen (it survives a close elsewhere).
  var isShown: ((String) -> Bool)?
  /// A kept entry's new ID.
  var onReplaced: ((String, String) -> Void)?
  private var running = false
  private var again = false

  let passwords: PasswordManager

  init(store: SpaceStore, history: HistoryStore, passwords: PasswordManager, directory: URL) {
    self.passwords = passwords
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
    if case .joined(let seed, let profiles) = result, let chain = try? Chain(seed: seed) {
      save(chain: chain, server: server)
      start(chain: chain, server: server, first: false, pairingProfiles: profiles)
    }
    return result
  }

  /// Joins with the 12 words. The chain's profiles come with the home
  /// account's first sync; their accounts start then.
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

  /// Stops syncing every profile's account; local data stays.
  func leave() {
    scheduled?.cancel()
    for account in accounts.values {
      account.listening?.cancel()
      spaceDirectory?.remove(account.spaces)
    }
    accounts = [:]
    syncedExtensions = []
    Keychain.set(nil, "seed")
    Keychain.set(nil, "server")
    for name in ["sync-metadata.plist", "sync-spaces-mirror.json", "sync-sessions.json", "sync-extensions.json", "Profiles"] {
      try? FileManager.default.removeItem(at: directory.appendingPathComponent(name))
    }
    spaceDirectory = nil
    chain = nil
    serverURL = nil
    devices = []
    profiles = []
    sessionsByAccount = [:]
    passwords.removeBridges()
    state = .off
    store.onLocalChange = nil
  }

  private func save(chain: Chain, server: URL) {
    Keychain.set(chain.seed, "seed")
    Keychain.set(server.dataRepresentation, "server")
  }

  /// Where an account keeps its files: the home's as before M8, the others
  /// under Profiles/<uuid>/.
  private func fileURL(_ name: String, profile: String) -> URL {
    if profile.isEmpty { return directory.appendingPathComponent(name) }
    let folder = directory.appendingPathComponent("Profiles/\(profile)", isDirectory: true)
    try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    return folder.appendingPathComponent(name)
  }

  private func start(chain: Chain, server: URL, first: Bool, pairingProfiles: [ChainProfile] = []) {
    self.chain = chain
    serverURL = server
    let spaceDirectory = SpaceDirectory(store: store)
    self.spaceDirectory = spaceDirectory
    startAccount("", first: first)
    spaceDirectory.adoptPairingProfiles(pairingProfiles)
    spaceDirectory.onProfilesChanged = { [weak self] list in self?.profilesChanged(list) }
    // The list the home account had (last run), else pairing's.
    let known = spaceDirectory.profiles.isEmpty ? ChainProfile.sorted(pairingProfiles) : spaceDirectory.profiles
    profilesChanged(known)
  }

  /// The chain's profile list changed: every profile but the home gets its
  /// account here (the phone doesn't create or delete chain profiles).
  private func profilesChanged(_ list: [ChainProfile]) {
    guard chain != nil else { return }
    profiles = list
    let home = ChainProfile.homeUUID(list) ?? store.homeProfileUUID
    // For the AutoFill extension's group headers.
    var names: [String: String] = [:]
    for p in list { names[p.uuid == home ? "" : p.uuid] = p.name }
    KeychainPasswordStore.saveProfileNames(names, accessGroup: passwords.group)
    var started = false
    for profile in list where profile.uuid != home && accounts[profile.uuid] == nil {
      startAccount(profile.uuid, first: false)
      started = true
    }
    if started { syncSoon(after: 0) }
  }

  private func startAccount(_ profile: String, first: Bool) {
    guard let chain, let server = serverURL, let spaceDirectory else { return }
    let metadataStore = FileSyncMetadataStore(url: fileURL("sync-metadata.plist", profile: profile))
    let cacheGUID = metadataStore.load()?.cacheGUID ?? SyncMetadata().cacheGUID
    if metadataStore.load() == nil { metadataStore.save(SyncMetadata(cacheGUID: cacheGUID)) }
    let authKey = chain.authKey(profileUUID: profile)
    let engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server, cacheGUID: cacheGUID, authKey: authKey),
      passphrase: chain.passphrase(profileUUID: profile), store: metadataStore, createChainIfEmpty: first)
    let spaces = CompassSpacesBridge(
      directory: spaceDirectory, profile: profile, engine: engine,
      mirrorURL: fileURL("sync-spaces-mirror.json", profile: profile))
    let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    let sessions = SessionsBridge(
      engine: engine, deviceName: UIDevice.current.name, stateURL: fileURL("sync-sessions.json", profile: profile))
    // Every account has this phone in its device list, so each profile's
    // page on the Mac shows it.
    let deviceInfo = DeviceInfoBridge(
      local: .init(name: UIDevice.current.name, model: UIDevice.current.model, appVersion: version))
    let account = Account(
      profile: profile, engine: engine, spaces: spaces, deviceInfo: deviceInfo,
      visits: HistoryBridge(history: history, engine: engine, profile: profile),
      directives: HistoryDeleteDirectivesBridge(history: history, engine: engine, profile: profile),
      sessions: sessions, passwords: PasswordsBridge(store: passwords.store(profile), engine: engine),
      extensions: ExtensionsBridge(stateURL: fileURL("sync-extensions.json", profile: profile)),
      sentTabs: SendTabBridge(
        engine: engine, cacheGUID: cacheGUID, stateURL: fileURL("sync-sent-tabs.json", profile: profile)),
      notifier: SyncNotifier(serverURL: server, cacheGUID: cacheGUID, authKey: authKey))
    if profile.isEmpty {
      deviceInfo.onChange = { [weak self] devices in
        Task { @MainActor in self?.devices = devices }
      }
    }
    sessions.onChange = { [weak self] _ in
      Task { @MainActor in self?.collectSessions() }
    }
    account.extensions.onChange = { [weak self] _ in
      Task { @MainActor in self?.collectExtensions() }
    }
    account.sentTabs.onReceived = { [weak self] tabs in
      Task { @MainActor in self?.onSentTabs?(tabs, profile) }
    }
    passwords.setBridge(account.passwords, for: profile)
    spaces.onLocalCommit = { [weak self] in self?.syncSoon(after: 2) }
    spaces.isShown = { [weak self] id in self?.isShown?(id) ?? false }
    spaces.onReplaced = { [weak self] old, new in self?.onReplaced?(old, new) }
    // Site icons other devices got from pages (docs/design/sync.md, "Favicons").
    spaces.onFavicon = { host, png in Favicons.shared.setSynced(host: host, png: png) }
    for (host, png) in spaces.syncedFavicons { Favicons.shared.setSynced(host: host, png: png) }
    accounts[profile] = account
    collectExtensions()
    if active { listen(account) }
    Task {
      for bridge: any DataTypeBridge in [
        spaces, deviceInfo, account.visits, account.directives, sessions, account.passwords, account.extensions,
        account.sentTabs,
      ] {
        await engine.register(bridge)
      }
      syncSoon(after: 0)
    }
  }

  /// Tabs sent to this phone, with the account they came in.
  var onSentTabs: (([SentTab], String) -> Void)?

  /// A sent tab was opened from its notification: its entry says so.
  func markSentTabOpened(_ guid: String, account: String) {
    guard let sentTabs = accounts[account]?.sentTabs else { return }
    Task {
      await sentTabs.markOpened(guid)
      syncSoon(after: 0)
    }
  }

  /// An icon from a page this phone loaded, for the account of the tab's
  /// space (written only if an entry there is on the host and it beats the
  /// account's).
  func offerFavicon(host: String, png: Data, sourceSize: Int, account: String) {
    accounts[account]?.spaces.offerFavicon(host: host, png: png, sourceSize: sourceSize)
  }

  /// The account a space's data syncs in.
  func accountProfile(of space: Space) -> String {
    if let owner = spaceDirectory?.owner(of: space) { return owner.accountProfile }
    return store.isHome(space.profileUUID) ? "" : space.profileUUID
  }

  /// "Tabs on <device>" of one account: the command bar shows the current
  /// space's profile only (M8).
  func foreignSessions(account: String) -> [ForeignSession] { sessionsByAccount[account] ?? [] }

  /// A profile account's name ("" = the home's).
  func profileName(account: String) -> String? {
    account.isEmpty ? profiles.first(where: \.home)?.name : profiles.first { $0.uuid == account }?.name
  }

  /// The account a profile's data syncs in: its own, or the home's.
  func accountProfile(of node: SidebarNode) -> String {
    if let owner = spaceDirectory?.owner(of: node) { return owner.accountProfile }
    let profile = store.profile(of: node)
    return store.isHome(profile) ? "" : profile
  }

  // MARK: Cycles

  /// The app came to the foreground or left it: hold the push streams only
  /// while active (iOS suspends background sockets anyway).
  func setActive(_ isActive: Bool) {
    active = isActive
    for account in accounts.values {
      if isActive {
        listen(account)
      } else {
        account.listening?.cancel()
        account.listening = nil
      }
    }
    if isActive { syncSoon() }
  }

  /// Each account has its own stream, as each desktop profile runs its own
  /// notifier.
  private func listen(_ account: Account) {
    guard account.listening == nil else { return }
    let notifier = account.notifier
    account.listening = Task { [weak self] in
      await notifier.run { _ in
        await MainActor.run { self?.syncSoon(after: 0.3) }
      }
    }
  }

  /// Runs a cycle after `delay` seconds (coalesced).
  func syncSoon(after delay: TimeInterval = 0) {
    guard !accounts.isEmpty else { return }
    scheduled?.cancel()
    scheduled = Task { [weak self] in
      if delay > 0 { try? await Task.sleep(for: .seconds(delay)) }
      guard !Task.isCancelled else { return }
      // Cancelling only stops the wait: a cycle that has started runs on
      // in its own task, or its requests would fail as "cancelled" when
      // the next syncSoon (for example from the push stream) comes in.
      await Task { await self?.syncNow() }.value
    }
  }

  /// One cycle of every account, the home's first (it brings the profile
  /// list, which may start more accounts).
  func syncNow() async {
    guard !accounts.isEmpty else { return }
    if running {
      again = true
      return
    }
    running = true
    state = .syncing
    defer { running = false }
    var failure: String?
    var poll: TimeInterval = .infinity
    var done: Set<String> = []
    while let profile = ([""] + accounts.keys.sorted()).first(where: { accounts[$0] != nil && !done.contains($0) }) {
      done.insert(profile)
      guard let account = accounts[profile] else { continue }
      if let message = await cycle(account) {
        failure = failure ?? message
        Logger(subsystem: "it.cipi1965.compass", category: "sync")
          .error("Sync failed (\(profile.isEmpty ? "home" : profile, privacy: .public)): \(message, privacy: .public)")
      }
      poll = min(poll, await account.engine.pollInterval ?? 30)
    }
    if let failure {
      state = .failed(failure)
    } else {
      lastSync = Date()
      state = .on
    }
    devices = accounts[""]?.deviceInfo.allDevices ?? []
    collectSessions()
    if again {
      again = false
      syncSoon(after: 0)
    } else {
      syncSoon(after: max(poll == .infinity ? 30 : poll, 15))
    }
  }

  /// One account's cycle; an error message if it failed.
  private func cycle(_ account: Account) async -> String? {
    do {
      await account.spaces.flush()
      try await account.engine.sync()
      await account.deviceInfo.refreshLocal(engine: account.engine)
      await account.sessions.publish(openTabs(account))
      await account.spaces.flush()
      if await account.engine.hasPendingChanges { try await account.engine.sync() }
      account.foreignSessions = account.sessions.foreignSessions()
      return nil
    } catch SyncEngineError.chainNotStarted {
      return account.profile.isEmpty
        ? "No device has started sync on this chain yet." : "A profile's sync hasn't started on the Mac yet."
    } catch SyncEngineError.crypto {
      return "This chain was reset on another device. Join it again."
    } catch SyncEngineError.connection(let error) {
      return "Can't reach the sync server (\(error))."
    } catch {
      return "\(error)"
    }
  }

  var recoveryWords: [String] { chain?.recoveryWords ?? [] }

  /// The Today tabs of the account's own spaces, for its SESSIONS.
  private func openTabs(_ account: Account) -> [SessionsBridge.LocalTab] {
    store.spaces.filter { spaceDirectory?.owner(of: $0) === account.spaces }
      .flatMap { $0.today.children.flatMap(\.tabs) }
      .filter { !$0.url.isEmpty }
      .sorted { ($0.lastActive ?? .distantPast) > ($1.lastActive ?? .distantPast) }
      .map { .init(id: $0.id, url: $0.url, title: $0.displayTitle, lastActive: $0.lastActive ?? Date()) }
  }

  /// Each account's other devices (a Mac's profile is a session in that
  /// profile's account only).
  private func collectExtensions() {
    var byID: [String: SyncedExtension] = [:]
    for account in accounts.values {
      for e in account.extensions.extensions where byID[e.id] == nil || e.enabled { byID[e.id] = e }
    }
    syncedExtensions = byID.values.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
  }

  private func collectSessions() {
    var out: [String: [ForeignSession]] = [:]
    for (profile, account) in accounts {
      if account.foreignSessions.isEmpty { account.foreignSessions = account.sessions.foreignSessions() }
      out[profile] = account.foreignSessions
    }
    sessionsByAccount = out
  }

  // MARK: History

  /// A page finished loading in a tab: a visit in the account of the tab's
  /// profile (synced when sync is on).
  func recordVisit(url: String, title: String, profile: String) {
    if let visits = accounts[profile]?.visits ?? accounts[""]?.visits {
      Task { await visits.record(url: url, title: title) }
      syncSoon(after: 5)
    } else {
      history.add(Visit(time: Visit.time(Date()), url: url, title: title, originator: "local", profile: profile))
    }
  }

  /// Clears history here, and in every profile's account: all of it, or
  /// what's newer than `since`.
  func clearHistory(since: Date? = nil) {
    let now = Date()
    let clearLocal = { [history] in
      if let since {
        history.delete(from: Visit.time(since), to: Visit.time(now))
      } else {
        history.deleteAll()
      }
    }
    guard !accounts.isEmpty else {
      clearLocal()
      return
    }
    let all = Array(accounts.values)
    Task {
      for account in all {
        await account.directives.deleteHistory(from: since ?? Date(timeIntervalSince1970: 0), to: now)
      }
      clearLocal()
      syncSoon()
    }
  }

  /// The chain profile a space belongs to.
  func profile(of space: Space) -> ChainProfile? {
    if store.isHome(space.profileUUID) { return profiles.first(where: \.home) }
    return profiles.first { $0.uuid == space.profileUUID }
  }

  /// How many spaces a chain profile has here.
  func spaceCount(_ profile: ChainProfile) -> Int {
    store.spaces.filter { self.profile(of: $0)?.uuid == profile.uuid }.count
  }

  /// The chain profile of a space, when the chain has more than one.
  func profileName(of space: Space) -> String? {
    profiles.count > 1 ? profile(of: space)?.name : nil
  }

  /// The profile UUID to give a space moved to `profile`.
  func spaceProfile(for profile: ChainProfile) -> String {
    profile.uuid == store.homeProfileUUID || profile.home ? store.homeProfileUUID : profile.uuid
  }
}
