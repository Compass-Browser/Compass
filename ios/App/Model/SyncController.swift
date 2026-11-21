// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AuthenticationServices
import CompassBoosts
import CompassClaude
import CompassCrypto
import CompassEasels
import CompassModel
import CompassShared
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
    /// Another device removed this one: the server refuses its cache GUIDs,
    /// so nothing syncs until it leaves and joins again (with new ones).
    case removed
    /// The server requires company sign-in and the chain isn't bound
    /// (docs/design/sync.md, "Company sign-in (OIDC)"): cycles and push
    /// streams wait until startLogin() succeeds.
    case loginRequired
  }

  /// Settings' words for `.removed`.
  static let removedMessage =
    "This device was removed from sync on another device. Its data stays here. Leave sync to set it up again."

  /// The server the join form starts with: the COMPASS_SYNC_SERVER build
  /// setting (ios/Local.xcconfig, not in the repository), through Info.plist.
  /// Builds without it start with an empty field.
  static let defaultServer: URL? = {
    guard let value = Bundle.main.object(forInfoDictionaryKey: "CompassSyncServer") as? String,
          !value.isEmpty else { return nil }
    return URL(string: value)
  }()

  /// One profile's server account: the home profile's is the chain's own
  /// ("" here); every other profile has its own (sync.md, "Profiles").
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
    /// For the push registration (docs/design/send-tab.md).
    var cacheGUID = ""
    var authKey = Data()
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
  /// Profile accounts (not the home's) that didn't sync in the last cycle,
  /// with why: one the Mac hasn't started yet, or a failure. They don't
  /// fail sync as a whole (SyncOutcome).
  private(set) var profileProblems: [String: String] = [:]
  /// Other devices' open tabs, per account ("" = the home's).
  private(set) var sessionsByAccount: [String: [ForeignSession]] = [:]
  /// Extensions on the chain's other devices (every profile's account),
  /// for Settings → Extensions to offer.
  private(set) var syncedExtensions: [SyncedExtension] = []
  var isOn: Bool { chain != nil }

  // Company sign-in (docs/design/ios.md, "Company sign-in").
  /// The server's identity provider ("Acme SSO"), when it requires sign-in.
  private(set) var loginProvider: String?
  /// Who the chain is signed in as, on a server with sign-in.
  private(set) var signedInUser: LoginClient.User?
  /// While signing in: the code the server's page shows too.
  private(set) var loginCode: String?
  /// Why the last sign-in didn't finish.
  private(set) var loginMessage: String?
  var isSigningIn: Bool { loginTask != nil }
  @ObservationIgnored private var loginTask: Task<Void, Never>?
  @ObservationIgnored private var webSession: ASWebAuthenticationSession?
  @ObservationIgnored private let webPresenter = LoginPresenter()
  @ObservationIgnored private var checkingAccount = false
  /// Profile accounts this run linked to the home's sign-in.
  @ObservationIgnored private var linkedProfiles: Set<String> = []

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
      start(chain: chain, server: server)
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
      start(chain: chain, server: server, pairingProfiles: profiles)
    }
    return result
  }

  /// Joins with the 12 words. The chain's profiles come with the home
  /// account's first sync; their accounts start then.
  func join(recoveryCode: String, server: URL) throws {
    let chain = try Chain(recoveryCode: recoveryCode)
    save(chain: chain, server: server)
    start(chain: chain, server: server)
  }

  /// The server URL as the desktop's ParseServerUrl() takes it: http or
  /// https, with a host; nil otherwise.
  static func parseServer(_ text: String) -> URL? {
    guard let url = URL(string: text.trimmingCharacters(in: .whitespacesAndNewlines)),
      url.scheme == "https" || url.scheme == "http", url.host()?.isEmpty == false
    else { return nil }
    return url
  }

  enum StartError: Error {
    case alreadySetUp
    case badServer

    /// The desktop's words (CompassSyncService::StartNewChain()).
    var message: String {
      switch self {
      case .alreadySetUp: "Sync is already set up."
      case .badServer: "Enter the server's URL, like https://sync.example.com/v2."
      }
    }
  }

  /// Starts a new chain from this device, as the desktop's "Start sync": a
  /// new seed, this device is the chain's home (its spaces, whatever their
  /// profile, sync in the home account: the phone doesn't create chain
  /// profiles), and the first cycle encrypts with the chain's passphrase
  /// before anything goes up (`SyncEngine`'s Nigori). Returns the 12 words
  /// to show once.
  func startNewChain(server text: String) -> Result<[String], StartError> {
    guard chain == nil else { return .failure(.alreadySetUp) }
    guard let server = Self.parseServer(text) else { return .failure(.badServer) }
    let chain = try! Chain(seed: Chain.generateSeed())
    save(chain: chain, server: server)
    start(chain: chain, server: server)
    return .success(chain.recoveryWords)
  }

  // MARK: Adding a device (pairing side A)

  /// Where "Add a Device" is, as the desktop's PairingStatus.
  struct Pairing: Equatable {
    enum Step { case none, working, showingCode, confirm, done, failed }
    var step = Step.none
    /// "7-482913".
    var code = ""
    /// The joining device's name, to confirm.
    var deviceName = ""
    var message = ""
    /// When the code stops working (the slot's end).
    var expiresAt: Date?
    /// The QR's text: `compass-pair:?server=…&code=…`.
    var invite: URL?
  }

  private(set) var pairing = Pairing()
  @ObservationIgnored private var host: PairingHost?
  @ObservationIgnored private var hostTask: Task<Void, Never>?

  /// Opens a pairing slot and shows its code, as the desktop's AddDevice().
  func addDevice() {
    guard host == nil, let chain else { return }
    guard let server = serverURL else {
      pairing = Pairing(step: .failed, message: "No server is set.")
      return
    }
    let host = PairingHost(
      transport: NetworkPairingTransport(serverURL: server, authKey: chain.authKey),
      host: server.host()?.lowercased() ?? "", seed: chain.seed, profiles: profiles)
    self.host = host
    pairing = Pairing(step: .working)
    hostTask = Task { [weak self] in
      let result = await host.run(
        onCode: { code, expiresIn in
          await MainActor.run {
            guard let self, self.host === host, let parsed = PairingCode(code) else { return }
            self.pairing = Pairing(
              step: .showingCode, code: code, message: "Enter this code on the other device.",
              expiresAt: Date().addingTimeInterval(expiresIn),
              invite: PairingInvite(server: server, code: parsed).url)
          }
        },
        onRequest: { name in
          await MainActor.run {
            guard let self, self.host === host else { return }
            self.pairing = Pairing(step: .confirm, code: self.pairing.code, deviceName: name)
          }
        })
      guard let self, self.host === host, !Task.isCancelled else { return }
      self.host = nil
      self.hostTask = nil
      self.pairing = Pairing(
        step: result == .added || result == .denied ? .done : .failed, message: Self.hostMessage(result))
      if result == .added { self.syncSoon(after: 5) }
    }
  }

  /// Allow or Deny for the device that asks to join.
  func answerPairing(allow: Bool) {
    guard let host, pairing.step == .confirm else { return }
    pairing = Pairing(step: .working)
    host.answer(allow: allow)
  }

  /// Cancel (or Close, once done): the slot closes if still open.
  func cancelPairing() {
    hostTask?.cancel()
    hostTask = nil
    host = nil
    pairing = Pairing()
  }

  /// The desktop's HostResultMessage().
  static func hostMessage(_ result: PairingHost.Result) -> String {
    switch result {
    case .added: "Device added. It starts syncing after a restart."
    case .denied: "Not added."
    case .wrongCode: "Wrong code. Try again with a new code."
    case .canceled: "The other device canceled."
    case .expired: "The code expired. Try again with a new code."
    case .rateLimited: "The server is busy. Try again in a few minutes."
    case .failed: "Couldn't reach the server."
    }
  }

  /// Stops syncing every profile's account; local data stays. Each
  /// account's metadata goes (sync-metadata.plist, Profiles/), so joining
  /// again gives every account a new cache GUID: a device removed elsewhere
  /// can come back that way.
  func leave() {
    cancelPairing()
    scheduled?.cancel()
    cancelLogin()
    // The server stops pushing to this phone (best effort; a removed
    // device's tokens are gone already).
    if let server = serverURL, state != .removed {
      for account in accounts.values {
        let (guid, key) = (account.cacheGUID, account.authKey)
        Task.detached { try? await Push.unregister(server: server, cacheGUID: guid, authKey: key) }
      }
      // The chain stays signed in: sign-in is per chain, and the other
      // devices on it keep syncing.
    }
    if let url = SharedContainer.group?.root.appendingPathComponent(Self.pushAccountsName) {
      try? FileManager.default.removeItem(at: url)
    }
    for account in accounts.values {
      account.listening?.cancel()
      spaceDirectory?.remove(account.spaces)
    }
    accounts = [:]
    syncedExtensions = []
    Keychain.set(nil, "seed")
    Keychain.set(nil, "server")
    for name in [
      "sync-metadata.plist", "sync-spaces-mirror.json", "sync-sessions.json", "sync-extensions.json",
      "sync-devices.plist", "Profiles",
    ] {
      try? FileManager.default.removeItem(at: directory.appendingPathComponent(name))
    }
    spaceDirectory = nil
    chain = nil
    serverURL = nil
    devices = []
    profiles = []
    profileProblems = [:]
    linking = []
    linked = []
    lastLink = [:]
    sessionsByAccount = [:]
    loginProvider = nil
    signedInUser = nil
    loginMessage = nil
    linkedProfiles = []
    passwords.removeBridges()
    RoomsModel.shared.syncStopped()
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

  private func start(chain: Chain, server: URL, pairingProfiles: [ChainProfile] = []) {
    self.chain = chain
    serverURL = server
    let spaceDirectory = SpaceDirectory(store: store)
    self.spaceDirectory = spaceDirectory
    // Sharing with other people (App/Rooms, docs/design/ios.md "Rooms").
    RoomsModel.shared.syncStarted(directory: spaceDirectory, stateDirectory: directory.appendingPathComponent("Rooms"))
    startAccount("")
    spaceDirectory.adoptPairingProfiles(pairingProfiles)
    spaceDirectory.onProfilesChanged = { [weak self] list in self?.profilesChanged(list) }
    // The list the home account had (last run), else pairing's.
    let known = spaceDirectory.profiles.isEmpty ? ChainProfile.sorted(pairingProfiles) : spaceDirectory.profiles
    profilesChanged(known)
    Task { await refreshLogin() }
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
      startAccount(profile.uuid)
      link(profile.uuid)
      started = true
    }
    if started { syncSoon(after: 0) }
  }

  // MARK: Linking profile accounts

  /// Profile accounts linked (or being linked) this run.
  @ObservationIgnored private var linking: Set<String> = []
  @ObservationIgnored private var linked: Set<String> = []
  @ObservationIgnored private var lastLink: [String: Date] = [:]

  /// Links a profile's account to the chain's on the server, once a run
  /// (again with `force`, after the server refused the account): a private
  /// server lets it in with the chain then (`AccountLinker`, sync.md
  /// "Profiles"). Best effort: a failure is tried again at the next 401.
  private func link(_ profile: String, force: Bool = false) {
    guard let chain, let server = serverURL, !profile.isEmpty, !linking.contains(profile),
      force || !linked.contains(profile),
      // After a refusal, at most once a minute.
      !force || Date().timeIntervalSince(lastLink[profile] ?? .distantPast) >= 60
    else { return }
    linking.insert(profile)
    lastLink[profile] = Date()
    Task { [weak self] in
      let result = await AccountLinker(serverURL: server).link(chain: chain, profileUUID: profile)
      guard let self, self.chain?.seed == chain.seed else { return }
      self.linking.remove(profile)
      if result == .linked {
        self.linked.insert(profile)
        // After a refusal, the account may sync now.
        if force { self.syncSoon(after: 1) }
      } else {
        Logger(subsystem: "it.cipi1965.compass", category: "sync")
          .error("Sync: linking a profile account failed: \(String(describing: result), privacy: .public)")
      }
    }
  }

  private func startAccount(_ profile: String) {
    guard let chain, let server = serverURL, let spaceDirectory else { return }
    let metadataStore = FileSyncMetadataStore(url: fileURL("sync-metadata.plist", profile: profile))
    let cacheGUID = metadataStore.load()?.cacheGUID ?? SyncMetadata().cacheGUID
    if metadataStore.load() == nil { metadataStore.save(SyncMetadata(cacheGUID: cacheGUID)) }
    let authKey = chain.authKey(profileUUID: profile)
    // Any device sets the chain's passphrase when the server has none, as
    // on the desktop (sync.md, "Passphrase"): the device that started the
    // chain (even after a relaunch before its first sync went through), or
    // one that finds a wiped server. Nothing goes up before it's in place.
    let engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server, cacheGUID: cacheGUID, authKey: authKey),
      passphrase: chain.passphrase(profileUUID: profile), store: metadataStore, createChainIfEmpty: true)
    let spaces = CompassSpacesBridge(
      directory: spaceDirectory, profile: profile, engine: engine,
      mirrorURL: fileURL("sync-spaces-mirror.json", profile: profile))
    let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "0"
    let sessions = SessionsBridge(
      engine: engine, deviceName: UIDevice.current.name, stateURL: fileURL("sync-sessions.json", profile: profile))
    // Every account has this phone in its device list, so each profile's
    // page on the Mac shows it.
    let deviceInfo = DeviceInfoBridge(
      local: .init(name: UIDevice.current.name, model: UIDevice.current.model, appVersion: version),
      cacheGUID: cacheGUID, stateURL: fileURL("sync-devices.plist", profile: profile))
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
      // Those known from the last run, before the first cycle.
      devices = deviceInfo.allDevices
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
    if profile.isEmpty {
      // Claude's memory goes with the home account (docs/design/claude.md,
      // "Memory").
      let memory = ClaudeMemoryStore.shared
      spaces.localMemories = { memory.entries }
      spaces.onMemories = { changed, deleted in memory.applyRemote(changed, deleted: deleted) }
      memory.onChange = { [weak spaces] entries in spaces?.memoriesChanged(entries) }
      // So do the Boosts (docs/design/boosts.md, "Sync").
      let boosts = BoostStore.shared
      spaces.localBoosts = { boosts.boosts }
      spaces.onBoosts = { changed, deleted in boosts.applyRemote(changed, deleted: deleted) }
      boosts.onChange = { [weak spaces] list in spaces?.boostsChanged(list) }
      // And the Easels (docs/design/easels.md, "Sync").
      let easels = EaselStore.shared
      spaces.localEasels = { easels.entities() }
      spaces.easelAssetData = { easels.assetData($0) }
      spaces.onEasels = { changed, deleted in easels.applyRemote(changed, deleted: deleted) }
      easels.onChange = { [weak spaces] in spaces?.easelsChanged(easels.entities()) }
    }
    account.cacheGUID = cacheGUID
    account.authKey = authKey
    accounts[profile] = account
    RoomsModel.shared.accountStarted(profile: profile, bridge: spaces, chain: chain, server: server, cacheGUID: cacheGUID)
    writePushAccounts()
    if let token = pushToken { registerPush(token, account: account) }
    collectExtensions()
    if active { listen(account) }
    Task {
      // The devices weren't kept before (sync-devices.plist): all of them
      // again, before the bridge is registered (so no cycle is under way
      // for the type).
      if deviceInfo.needsRefetch { await engine.refetch(.deviceInfo) }
      for bridge: any DataTypeBridge in [
        spaces, deviceInfo, account.visits, account.directives, sessions, account.passwords, account.extensions,
        account.sentTabs,
      ] {
        await engine.register(bridge)
      }
      syncSoon(after: 0)
    }
  }

  // MARK: Push

  /// The APNs device token, once iOS gave one.
  @ObservationIgnored private var pushToken: Data?
  static let pushAccountsName = "PushAccounts.json"

  /// A new (or the same) device token: every account registers it.
  func setPushToken(_ token: Data) {
    pushToken = token
    for account in accounts.values { registerPush(token, account: account) }
  }

  private func registerPush(_ token: Data, account: Account) {
    guard let server = serverURL, state != .removed else { return }
    #if DEBUG
      let environment = Push.Environment.sandbox
    #else
      let environment = Push.Environment.production
    #endif
    let (guid, key) = (account.cacheGUID, account.authKey)
    Task.detached {
      do {
        try await Push.register(token: token, environment: environment, server: server, cacheGUID: guid, authKey: key)
      } catch {
        #if DEBUG
          print("Push: register failed: \(error)")
        #endif
      }
    }
  }

  /// The accounts' cache GUIDs, for the notification service extension
  /// (it reads the seed and server from the shared Keychain group).
  private func writePushAccounts() {
    guard let url = SharedContainer.group?.root.appendingPathComponent(Self.pushAccountsName) else { return }
    let list = accounts.values.map { PushAccount(profile: $0.profile, cacheGUID: $0.cacheGUID) }
      .sorted { $0.profile < $1.profile }
    try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try? JSONEncoder().encode(list).write(to: url, options: .atomic)
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

  /// The devices of an account a tab can be sent to (Claude's
  /// send_tab_to_device); nil without that account.
  func sendTargets(account: String) -> [SyncDevice]? {
    accounts[account]?.deviceInfo.sendTargets()
  }

  /// Sends a page to another device of the account, as the desktop's "Send
  /// to your devices" does: it shows there as a notification.
  func sendTab(url: String, title: String, to device: SyncDevice, account: String) {
    guard let account = accounts[account] else { return }
    let sentTabs = account.sentTabs
    let name = account.deviceInfo.localName
    let target = device.cacheGUID
    Task {
      await sentTabs.send(url: url, title: title, deviceName: name, to: target)
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

  // MARK: Removing a device

  /// Removes another device from sync (docs/design/ios.md, "Removing a
  /// device"): the server deletes its entry in the home account's
  /// DEVICE_INFO, which the other devices drop with their next sync, and
  /// refuses it from then on. Only the home account's entry: a profile
  /// account lists the same computer under another cache GUID, and nothing
  /// in DEVICE_INFO ties the two together (signin_scoped_device_id is per
  /// profile, and names repeat), so those entries stay until that device
  /// stops syncing them, as it does once it learns it was removed.
  func removeDevice(_ device: SyncDevice) async throws {
    guard !device.isThisDevice else { throw DeviceRemover.Failure.thisDevice }
    guard let server = serverURL, let home = accounts[""] else { throw DeviceRemover.Failure.network("Sync is off.") }
    let remover = DeviceRemover(serverURL: server, cacheGUID: home.cacheGUID, authKey: home.authKey)
    do {
      try await remover.remove(device.cacheGUID)
    } catch DeviceRemover.Failure.deviceRemoved {
      deviceRemoved()
      throw DeviceRemover.Failure.deviceRemoved
    }
    // Gone from the list now; the tombstone comes with the next cycle.
    home.deviceInfo.forget(device.cacheGUID)
    devices = home.deviceInfo.allDevices
    syncSoon(after: 0)
  }

  /// The server said another device removed this one: everything stops
  /// (cycles, streams, push) until Leave. The data stays.
  private func deviceRemoved() {
    guard chain != nil, state != .removed else { return }
    Logger(subsystem: "it.cipi1965.compass", category: "sync")
      .error("Sync: this device was removed on another device")
    state = .removed
    scheduled?.cancel()
    scheduled = nil
    cancelPairing()
    for account in accounts.values {
      account.listening?.cancel()
      account.listening = nil
    }
  }

  // MARK: Cycles

  /// The app came to the foreground or left it: hold the push streams only
  /// while active (iOS suspends background sockets anyway).
  func setActive(_ isActive: Bool) {
    active = isActive
    RoomsModel.shared.setActive(isActive && state != .removed)
    if isActive, state == .loginRequired {
      // Another device of the chain may have signed it in meanwhile.
      Task { await refreshLogin() }
      return
    }
    for account in accounts.values {
      if isActive, state != .removed {
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
    guard account.listening == nil, state != .loginRequired else { return }
    let notifier = account.notifier
    let profile = account.profile
    account.listening = Task { [weak self] in
      await notifier.run(
        changed: { _ in await MainActor.run { self?.syncSoon(after: 0.3) } },
        removed: { await MainActor.run { self?.deviceRemoved() } },
        refused: { loginRequired in
          guard loginRequired else { return }
          // The sign-in gate: no retrying every minute; a cycle finds out
          // what's needed (sign in, or link a profile account), and the
          // stream starts again after a good one.
          await MainActor.run {
            let account = self?.accounts[profile]
            account?.listening?.cancel()
            account?.listening = nil
            self?.syncSoon(after: 0)
          }
        },
        room: { [profile = account.profile] event in await MainActor.run { RoomsModel.shared.handle(event, account: profile) } })
    }
  }

  /// Runs a cycle after `delay` seconds (coalesced).
  func syncSoon(after delay: TimeInterval = 0) {
    guard !accounts.isEmpty, state != .removed, state != .loginRequired else { return }
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
    guard !accounts.isEmpty, state != .removed else { return }
    if state == .loginRequired {
      // Sync Now while waiting for sign-in: ask the server again, once.
      await refreshLogin()
      return
    }
    if running {
      again = true
      return
    }
    running = true
    state = .syncing
    defer { running = false }
    var results: [String: SyncOutcome.AccountResult] = [:]
    var poll: TimeInterval = .infinity
    var done: Set<String> = []
    // 401s: the accounts the server's sign-in gate stopped, and whether one
    // was a key it refuses.
    var gated: [String] = []
    var refusedKey = false
    // One account's failure doesn't stop the others: each runs, and only the
    // home's decides the state (SyncOutcome).
    while let profile = ([""] + accounts.keys.sorted()).first(where: { accounts[$0] != nil && !done.contains($0) }) {
      done.insert(profile)
      guard let account = accounts[profile] else { continue }
      let cycled = await cycle(account)
      if state == .removed { return }
      let result: SyncOutcome.AccountResult
      switch cycled {
      case .done(let r):
        result = r
      case .loginRequired:
        gated.append(profile)
        result = .failed("The sync server asks this account to sign in.")
      case .refusedKey(let r):
        refusedKey = true
        result = r
      }
      results[profile] = result
      if case .failed(let message) = result {
        Logger(subsystem: "it.cipi1965.compass", category: "sync")
          .error("Sync failed (\(profile.isEmpty ? "home" : profile, privacy: .public)): \(message, privacy: .public)")
      }
      poll = min(poll, await account.engine.pollInterval ?? 30)
    }
    if !gated.isEmpty || refusedKey {
      // A 401: the server's sign-in gate, or a key it refuses. /account
      // tells them apart (it isn't gated).
      switch await checkAccount(gated: gated) {
      case .loginRequired:
        again = false
        return
      case .linked:
        again = true
      case .signedIn, .unknown:
        break
      }
    }
    let outcome = SyncOutcome(results: results)
    profileProblems = outcome.profileProblems
    if let failure = outcome.failure {
      state = .failed(failure)
    } else {
      lastSync = Date()
      state = .on
      if active { for account in accounts.values { listen(account) } }
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

  private enum CycleResult {
    case done(SyncOutcome.AccountResult)
    /// 401 from the sign-in gate.
    case loginRequired
    /// 401 without it: the server doesn't take the account's key.
    case refusedKey(SyncOutcome.AccountResult)
  }

  /// One account's cycle.
  private func cycle(_ account: Account) async -> CycleResult {
    do {
      await account.spaces.flush()
      try await account.engine.sync()
      await account.deviceInfo.refreshLocal(engine: account.engine)
      await account.sessions.publish(openTabs(account))
      await account.spaces.flush()
      if await account.engine.hasPendingChanges { try await account.engine.sync() }
      account.foreignSessions = account.sessions.foreignSessions()
      return .done(.synced)
    } catch SyncEngineError.chainNotStarted {
      return .done(.notStarted)
    } catch SyncEngineError.connection(.loginRequired) {
      return .loginRequired
    } catch SyncEngineError.connection(.http(401)) {
      // A private server that doesn't know this profile's account: linking
      // it from the home account lets it in (sync.md, "Profiles").
      if !account.profile.isEmpty { link(account.profile, force: true) }
      return .refusedKey(
        .failed(account.profile.isEmpty ? SyncOutcome.homeRefusedMessage : SyncOutcome.profileRefusedMessage))
    } catch SyncEngineError.connection(.deviceRemoved) {
      deviceRemoved()
      return .done(.synced)
    } catch SyncEngineError.crypto {
      return .done(.failed("This chain was reset on another device. Join it again."))
    } catch SyncEngineError.connection(let error) {
      return .done(.failed("Can't reach the sync server (\(error))."))
    } catch {
      return .done(.failed("\(error)"))
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

  // MARK: Company sign-in

  private enum AccountCheck {
    /// The chain must sign in: now in `.loginRequired`.
    case loginRequired
    /// Signed in; gated profile accounts were linked to it, sync again.
    case linked
    /// Signed in (or no sign-in), nothing to link.
    case signedIn
    /// The server didn't answer.
    case unknown
  }

  /// After a 401: asks /account with the home chain's key. A chain that
  /// must sign in stops syncing until it does; a signed-in chain links the
  /// gated profile accounts (each is a chain of its own on the server; the
  /// Mac links them when it adds a profile, but a sign-in from this phone,
  /// say after a revoke, binds only the home chain).
  private func checkAccount(gated: [String]) async -> AccountCheck {
    guard let chain, let server = serverURL, !checkingAccount else { return .unknown }
    checkingAccount = true
    defer { checkingAccount = false }
    let client = LoginClient(serverURL: server, authKey: chain.authKey)
    guard let account = try? await client.account(), self.chain?.seed == chain.seed else { return .unknown }
    signedInUser = account.user
    if account.loginRequired {
      if loginProvider == nil { loginProvider = (try? await LoginClient.info(serverURL: server)) ?? "" }
      enterLoginRequired()
      return .loginRequired
    }
    var linked = false
    for profile in gated where !profile.isEmpty && !linkedProfiles.contains(profile) {
      guard let key = accounts[profile]?.authKey else { continue }
      linkedProfiles.insert(profile)
      if (try? await client.link(profileAuthKey: key)) != nil { linked = true }
    }
    return linked ? .linked : .signedIn
  }

  /// Asks the server whether it has sign-in and who the chain is signed in
  /// as (on start, on return to the foreground while waiting, Sync Now).
  func refreshLogin() async {
    guard let chain, let server = serverURL else { return }
    do {
      loginProvider = try await LoginClient.info(serverURL: server)
    } catch {
      // Unreachable: keep what we knew.
    }
    guard self.chain?.seed == chain.seed else { return }
    guard loginProvider != nil else {
      signedInUser = nil
      if state == .loginRequired { resume() }  // the server dropped sign-in
      return
    }
    guard let account = try? await LoginClient(serverURL: server, authKey: chain.authKey).account(),
      self.chain?.seed == chain.seed
    else { return }
    signedInUser = account.user
    if account.loginRequired {
      enterLoginRequired()
    } else if state == .loginRequired {
      resume()
    }
  }

  /// Stops hammering the server: no cycles, no push streams.
  private func enterLoginRequired() {
    guard state != .loginRequired else { return }
    state = .loginRequired
    scheduled?.cancel()
    scheduled = nil
    for account in accounts.values {
      account.listening?.cancel()
      account.listening = nil
    }
  }

  /// Signed in: sync again, with the push streams and registrations.
  private func resume() {
    if state == .loginRequired { state = .syncing }
    linkedProfiles = []
    if let token = pushToken {
      for account in accounts.values { registerPush(token, account: account) }
    }
    if active { for account in accounts.values { listen(account) } }
    syncSoon(after: 0)
  }

  /// Signs the chain in with the server's identity provider: the server's
  /// page in an ASWebAuthenticationSession (the shared browser session, so
  /// a company SSO cookie in Safari is used), while this polls the server
  /// for the result. The page shows `loginCode` too.
  func startLogin() {
    guard let chain, let server = serverURL, loginTask == nil else { return }
    loginMessage = nil
    let client = LoginClient(serverURL: server, authKey: chain.authKey)
    loginTask = Task { [weak self] in
      await self?.runLogin(client)
      self?.finishLogin()
    }
  }

  /// Stops a sign-in in progress (the sheet closes; the server's pending
  /// login expires on its own).
  func cancelLogin() {
    loginTask?.cancel()
    finishLogin()
  }

  private func finishLogin() {
    loginTask = nil
    loginCode = nil
    let session = webSession
    webSession = nil
    session?.cancel()
  }

  private func runLogin(_ client: LoginClient) async {
    let request: LoginClient.Request
    do {
      request = try await client.start(deviceName: UIDevice.current.name)
    } catch LoginClient.LoginError.disabled {
      // The server no longer requires sign-in.
      loginProvider = nil
      await refreshLogin()
      return
    } catch LoginClient.LoginError.unavailable {
      loginMessage = "Sign-in isn't available on the server right now. Try again in a minute."
      return
    } catch LoginClient.LoginError.rateLimited {
      loginMessage = "Too many sign-in attempts. Wait a few minutes."
      return
    } catch {
      if !Task.isCancelled { loginMessage = "Couldn't reach the sync server." }
      return
    }
    guard !Task.isCancelled else { return }
    loginCode = request.userCode
    present(request.url)
    var failures = 0
    while !Task.isCancelled {
      do {
        let status = try await client.poll(request.id)
        failures = 0
        guard !Task.isCancelled else { return }
        switch status {
        case .pending:
          if Date() > request.expires.addingTimeInterval(10) {
            loginMessage = "The sign-in expired. Try again."
            return
          }
        case .done(let user):
          signedInUser = user
          loginProvider = loginProvider ?? ""
          finishLogin()
          resume()
          return
        case .denied:
          loginMessage = "Sign-in was denied."
          return
        case .expired:
          loginMessage = "The sign-in expired. Try again."
          return
        }
      } catch {
        if Task.isCancelled { return }
        failures += 1
        if failures >= 5 {
          loginMessage = "Lost the sync server while signing in. Try again."
          return
        }
        try? await Task.sleep(for: .seconds(2))
      }
    }
  }

  /// The server's page in the system's web authentication sheet. With
  /// `return: "compass"`, Allow or Deny ends in compass://sync-login-done,
  /// which closes the sheet; the poll has the answer by then.
  private func present(_ url: URL) {
    let completion: ASWebAuthenticationSession.CompletionHandler = { [weak self] _, error in
      Task { @MainActor in
        guard let self, self.webSession != nil else { return }
        self.webSession = nil
        // Closed by the person before the server had an answer.
        if (error as? ASWebAuthenticationSessionError)?.code == .canceledLogin { self.cancelLogin() }
      }
    }
    let session: ASWebAuthenticationSession
    if #available(iOS 17.4, *) {
      session = ASWebAuthenticationSession(
        url: url, callback: .customScheme(LoginClient.callbackScheme), completionHandler: completion)
    } else {
      session = ASWebAuthenticationSession(
        url: url, callbackURLScheme: LoginClient.callbackScheme, completionHandler: completion)
    }
    session.prefersEphemeralWebBrowserSession = false
    session.presentationContextProvider = webPresenter
    webSession = session
    if !session.start() {
      webSession = nil
      loginMessage = "Couldn't open the sign-in page."
      loginTask?.cancel()
    }
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

  /// Why a chain profile's account didn't sync last time; nil if it did.
  func problem(_ profile: ChainProfile) -> String? {
    profile.uuid == ChainProfile.homeUUID(profiles) ? nil : profileProblems[profile.uuid]
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

/// Where the sign-in sheet shows: the app's front window.
private final class LoginPresenter: NSObject, ASWebAuthenticationPresentationContextProviding {
  func presentationAnchor(for session: ASWebAuthenticationSession) -> ASPresentationAnchor {
    MainActor.assumeIsolated {
      let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
      let scene = scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
      return scene?.keyWindow ?? scene?.windows.first ?? ASPresentationAnchor()
    }
  }
}
