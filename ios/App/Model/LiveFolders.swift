// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassLiveFolders
import CompassModel
import Foundation
import Observation
import Security
import WebKit

/// Live folders on the phone (docs/design/live-folders.md, "iOS"): the
/// plugins (built in, and installed from a manifest's address), each
/// folder's refresh state and schedule, secrets in the Keychain and the
/// folders allowed to send cookies. Refreshes run on CompassLiveFolders'
/// runtime and land in the store with `replaceLiveItems`, which keeps them
/// out of sync. The desktop's LiveFolderService.
@MainActor
@Observable
final class LiveFolders {
  struct State {
    var refreshing = false
    /// The last refresh's error, shown on the folder.
    var error: LiveError?
    var failures = 0
    var lastAttempt: Date?
    var nextRefresh: Date?
    /// Created without a name: the plugin's suggested title names it.
    var titleFromPlugin = false
    var log: [String] = []
  }

  /// Refreshes no sooner than this after the last try when a folder opens
  /// or the app comes back.
  static let staleAfter: TimeInterval = 5 * 60
  static let maxInterval: TimeInterval = 24 * 60 * 60
  static let firstRetry: TimeInterval = 5 * 60
  static let maxConcurrent = 2

  private(set) var states: [String: State] = [:]
  /// Built-ins first, then installed plugins, by name.
  private(set) var plugins: [LivePlugin] = []

  private let store: SpaceStore
  private let tabs: TabManager
  private let scripts: LiveScripts?
  let rules: LiveRules?
  private let pluginsDirectory: URL
  private let settingsURL: URL
  @ObservationIgnored private var cookieGrants: Set<String> = []
  @ObservationIgnored private var queue: [String] = []
  @ObservationIgnored private var running = 0
  @ObservationIgnored private var timer: Task<Void, Never>?
  @ObservationIgnored private var waiters: [CheckedContinuation<Void, Never>] = []

  init(store: SpaceStore, tabs: TabManager, directory: URL) {
    self.store = store
    self.tabs = tabs
    pluginsDirectory = directory.appendingPathComponent("Live Plugins", isDirectory: true)
    settingsURL = directory.appendingPathComponent("Live Folders.json")
    // The repo's plugins/ folder, copied into the app (project.yml).
    scripts = Bundle.main.url(forResource: "plugins", withExtension: nil).flatMap { try? LiveScripts(directory: $0) }
    rules = scripts.map(LiveRules.init(scripts:))
    if let data = try? Data(contentsOf: settingsURL),
      let saved = try? JSONDecoder().decode(Settings.self, from: data)
    {
      cookieGrants = Set(saved.cookieGrants)
    }
    reloadPlugins()
  }

  private struct Settings: Codable {
    var cookieGrants: [String]
  }

  private func saveSettings() {
    let data = try? JSONEncoder().encode(Settings(cookieGrants: cookieGrants.sorted()))
    try? data?.write(to: settingsURL, options: .atomic)
  }

  // MARK: Plugins

  func reloadPlugins() {
    guard let scripts, let rules else { return }
    let installed = LivePluginPackage.installed(in: pluginsDirectory, rules: rules)
      .filter { !$0.isBuiltin }.sorted { $0.manifest.name.localizedStandardCompare($1.manifest.name) == .orderedAscending }
    plugins = scripts.builtins() + installed
  }

  func plugin(id: String) -> LivePlugin? { plugins.first { $0.id == id } }

  /// A folder's plugin, if this device has it (the same files, for an
  /// installed one: another version is used anyway, as on the desktop).
  func plugin(for folder: SidebarNode) -> LivePlugin? {
    folder.liveSource.flatMap { plugin(id: $0.pluginID) }
  }

  /// Fetches a plugin to review before installing it.
  func fetchPackage(_ address: String) async throws -> LivePluginPackage {
    guard let rules else { throw LiveError("bad_plugin", "Live folders aren't available.") }
    let trimmed = address.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let url = URL(string: trimmed) else { throw LiveError("bad_request", "That isn't an address.") }
    return try await LivePluginPackage.fetch(url, rules: rules)
  }

  func install(_ package: LivePluginPackage) throws {
    try FileManager.default.createDirectory(at: pluginsDirectory, withIntermediateDirectories: true)
    _ = try package.install(in: pluginsDirectory)
    reloadPlugins()
    // Folders that waited for it.
    for folder in liveFolders where folder.liveSource?.pluginID == package.manifest.id { refresh(folder) }
  }

  func remove(_ plugin: LivePlugin) {
    guard !plugin.isBuiltin else { return }
    try? FileManager.default.removeItem(at: pluginsDirectory.appendingPathComponent(plugin.id, isDirectory: true))
    reloadPlugins()
  }

  // MARK: Secrets and cookies (this device only)

  /// Secrets are per plugin and profile, as on the desktop (per profile
  /// there), in the Keychain, never synced or backed up.
  private static let keychainService = "it.cipi1965.compass.live-folders"

  private func account(_ plugin: String, _ key: String, profile: String) -> String {
    "\(profile.isEmpty ? "home" : profile)/\(plugin)/\(key)"
  }

  func secret(_ key: String, plugin: String, profile: String) -> String? {
    var query = keychainQuery(account(plugin, key, profile: profile))
    query[kSecReturnData as String] = true
    query[kSecMatchLimit as String] = kSecMatchLimitOne
    var result: CFTypeRef?
    guard SecItemCopyMatching(query as CFDictionary, &result) == errSecSuccess, let data = result as? Data else {
      return nil
    }
    return String(data: data, encoding: .utf8)
  }

  /// An empty value removes it.
  func setSecret(_ key: String, plugin: String, profile: String, value: String) {
    let query = keychainQuery(account(plugin, key, profile: profile))
    SecItemDelete(query as CFDictionary)
    guard !value.isEmpty else { return }
    var add = query
    add[kSecValueData as String] = Data(value.utf8)
    add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
    SecItemAdd(add as CFDictionary, nil)
  }

  private func keychainQuery(_ account: String) -> [String: Any] {
    [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: Self.keychainService,
      kSecAttrAccount as String: account,
    ]
  }

  private func secrets(of plugin: LivePlugin, for folder: SidebarNode) -> [String: String] {
    let profile = store.profile(of: folder)
    var out: [String: String] = [:]
    for s in plugin.manifest.secrets {
      if let value = secret(s.key, plugin: plugin.id, profile: profile) { out[s.key] = value }
    }
    return out
  }

  func cookiesGranted(_ folder: SidebarNode) -> Bool { cookieGrants.contains(folder.id) }

  func setCookiesGranted(_ folder: SidebarNode, _ granted: Bool) {
    if granted { cookieGrants.insert(folder.id) } else { cookieGrants.remove(folder.id) }
    saveSettings()
  }

  // MARK: Folders

  /// Every live folder of the sidebar.
  var liveFolders: [SidebarNode] {
    store.spaces.flatMap { $0.pinned.subtree }.filter(\.isLiveFolder)
  }

  /// A new live folder at the end of `parent`, refreshed at once. An empty
  /// name takes the plugin's (and then the title its result suggests).
  @discardableResult
  func create(
    plugin: LivePlugin, name: String, config: [String: Any], secrets: [String: String], grantCookies: Bool,
    in parent: SidebarNode
  ) -> SidebarNode? {
    let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
    let source = LiveSource(
      pluginID: plugin.id, configJSON: LiveSource.json(config), pluginURL: plugin.sourceURL,
      pluginSHA256: plugin.sha256)
    guard let folder = store.addLiveFolder(name: trimmed.isEmpty ? plugin.manifest.name : trimmed, source: source, to: parent)
    else { return nil }
    saveSecrets(secrets, plugin: plugin, folder: folder)
    setCookiesGranted(folder, grantCookies)
    states[folder.id, default: State()].titleFromPlugin = trimmed.isEmpty
    refresh(folder)
    return folder
  }

  /// Edit Source: new settings, refreshed at once.
  func update(_ folder: SidebarNode, plugin: LivePlugin, config: [String: Any], secrets: [String: String], grantCookies: Bool) {
    store.setLiveSource(
      folder,
      LiveSource(
        pluginID: plugin.id, configJSON: LiveSource.json(config), pluginURL: plugin.sourceURL,
        pluginSHA256: plugin.sha256))
    saveSecrets(secrets, plugin: plugin, folder: folder)
    setCookiesGranted(folder, grantCookies)
    refresh(folder)
  }

  /// Blank secrets keep the saved value.
  private func saveSecrets(_ secrets: [String: String], plugin: LivePlugin, folder: SidebarNode) {
    let profile = store.profile(of: folder)
    for (key, value) in secrets where !value.isEmpty && plugin.manifest.secrets.contains(where: { $0.key == key }) {
      setSecret(key, plugin: plugin.id, profile: profile, value: value)
    }
  }

  func convertToPlain(_ folder: SidebarNode) {
    store.convertToPlainFolder(folder)
    states[folder.id] = nil
    cookieGrants.remove(folder.id)
    saveSettings()
  }

  // MARK: Refreshing

  /// The folder was opened: a refresh unless one was tried lately.
  func expanded(_ folder: SidebarNode) {
    let state = states[folder.id]
    if let last = state?.lastAttempt, Date().timeIntervalSince(last) < Self.staleAfter { return }
    if state == nil, let refreshed = folder.liveRefreshed, Date().timeIntervalSince(refreshed) < Self.staleAfter {
      return
    }
    refresh(folder)
  }

  /// Queues a refresh.
  func refresh(_ folder: SidebarNode) {
    guard folder.isLiveFolder, !queue.contains(folder.id), states[folder.id]?.refreshing != true else { return }
    queue.append(folder.id)
    pump()
  }

  /// When a folder is due: its schedule, or (first time this launch) its
  /// last refresh plus the plugin's default interval.
  private func due(_ folder: SidebarNode, now: Date) -> Bool {
    if let next = states[folder.id]?.nextRefresh { return next <= now }
    guard let refreshed = folder.liveRefreshed else { return true }
    let interval = plugin(for: folder).map { $0.manifest.refresh.default } ?? Self.staleAfter
    return now.timeIntervalSince(refreshed) >= min(interval, Self.maxInterval)
  }

  /// Every folder that's due.
  func refreshDue(now: Date = Date()) {
    for folder in liveFolders where due(folder, now: now) && plugin(for: folder) != nil { refresh(folder) }
  }

  /// The app is in the foreground: refresh what's due now and keep
  /// checking; in the background the BGAppRefreshTask does it.
  func setActive(_ active: Bool) {
    timer?.cancel()
    timer = nil
    guard active else { return }
    refreshDue()
    timer = Task { [weak self] in
      while !Task.isCancelled {
        try? await Task.sleep(for: .seconds(30))
        self?.refreshDue()
      }
    }
  }

  /// The background refresh: what's due, until it's all done.
  func refreshDueAndWait() async {
    refreshDue()
    guard running > 0 || !queue.isEmpty else { return }
    await withCheckedContinuation { waiters.append($0) }
  }

  private func pump() {
    while running < Self.maxConcurrent, !queue.isEmpty {
      let id = queue.removeFirst()
      guard let folder = store.node(id: id), folder.isLiveFolder else { continue }
      running += 1
      Task { await run(folder) }
    }
    if running == 0, queue.isEmpty {
      let waiting = waiters
      waiters.removeAll()
      for w in waiting { w.resume() }
    }
  }

  private func run(_ folder: SidebarNode) async {
    defer {
      running -= 1
      pump()
    }
    let id = folder.id
    var state = states[id] ?? State()
    state.lastAttempt = Date()
    guard let source = folder.liveSource, let scripts else { return }
    guard let plugin = plugin(id: source.pluginID) else {
      state.error = LiveError(
        "bad_plugin",
        source.pluginURL.isEmpty
          ? "This folder's plugin isn't on this device." : "This folder's plugin isn't installed on this device.")
      state.nextRefresh = nil
      states[id] = state
      return
    }
    state.refreshing = true
    states[id] = state

    let started = Date()
    let grant = cookiesGranted(folder)
    let fetcher = URLSessionLiveFetcher { [weak self] url in await self?.cookieHeader(for: url, folder: id) ?? [:] }
    let outcome = await LiveRuntime(scripts: scripts).run(
      plugin, configJSON: source.configJSON, secrets: secrets(of: plugin, for: folder), grantCookies: grant,
      environment: LiveEnvironment(now: started), fetcher: fetcher)

    state = states[id] ?? state
    state.refreshing = false
    state.log = outcome.log
    guard let current = store.node(id: id), current.isLiveFolder else {
      states[id] = nil
      return
    }
    switch outcome.result {
    case .success(let result):
      store.replaceLiveItems(
        current, items: result.items.map { LiveItem(key: $0.id, url: $0.url, title: $0.title) }, now: Date(),
        isOpen: { [tabs] in tabs.isLive($0) })
      if state.titleFromPlugin, let title = result.title, !title.isEmpty { store.rename(current, customTitle: title) }
      state.titleFromPlugin = false
      state.error = nil
      state.failures = 0
      let interval = min(
        max(result.nextRefresh ?? plugin.manifest.refresh.default, plugin.manifest.refresh.min), Self.maxInterval)
      state.nextRefresh = started.addingTimeInterval(interval)
    case .failure(let error):
      state.error = error
      state.failures += 1
      var delay = Self.firstRetry
      for _ in 1..<max(1, state.failures) where delay < Self.maxInterval { delay *= 2 }
      state.nextRefresh = Date().addingTimeInterval(min(delay, Self.maxInterval))
    }
    states[id] = state
  }

  /// The cookies a URL would get in the website data of the folder's space
  /// (asked only for the plugin's cookie hosts, once the user allowed it).
  private func cookieHeader(for url: URL, folder id: String) async -> [String: String] {
    guard let folder = store.node(id: id), cookiesGranted(folder) else { return [:] }
    let all = await tabs.dataStore(of: folder).httpCookieStore.allCookies()
    guard let host = url.host()?.lowercased() else { return [:] }
    let path = url.path.isEmpty ? "/" : url.path
    let now = Date()
    let cookies = all.filter { cookie in
      var domain = cookie.domain.lowercased()
      if domain.hasPrefix(".") { domain.removeFirst() }
      guard host == domain || host.hasSuffix("." + domain) else { return false }
      guard path.hasPrefix(cookie.path) else { return false }
      if cookie.isSecure && url.scheme != "https" { return false }
      if let expires = cookie.expiresDate, expires < now { return false }
      return true
    }
    return HTTPCookie.requestHeaderFields(with: cookies)
  }
}
