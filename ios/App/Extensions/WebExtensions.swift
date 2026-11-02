// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Observation
import SwiftUI
import UIKit
import WebKit

/// Web extensions (docs/design/ios.md, "Web extensions"): Chrome and Firefox
/// extensions run by WebKit's WKWebExtension API (iOS 18.4+). Each one is
/// unpacked in Application Support/Compass/Extensions/<id> (ExtensionPackage),
/// with Extensions.json listing them; one WKWebExtensionController runs the
/// enabled ones, and every tab's web view is attached to it, so content
/// scripts, content blockers and the background page reach every profile.
///
/// To extensions, each space is a window and its entries are its tabs (an
/// entry without a web view is an unloaded tab); the shown space is the
/// focused window.
///
/// On older systems the class exists but does nothing (`isSupported`).
@MainActor
@Observable
final class WebExtensions: NSObject {
  struct Record: Codable, Identifiable, Equatable {
    /// The context's unique identifier: the host of its pages, the key of
    /// its storage.
    let id: String
    var source: ExtensionSource
    var enabled = true
    var installed = Date()
    /// Optional permissions and sites granted at the extension's request.
    var grantedOptional: [String] = []
    var grantedOptionalPatterns: [String] = []
    /// A newer version that asks for more than this one: it waits for the
    /// user ("Review Update").
    var availableUpdate: String?
    /// Only these hosts (and their subdomains) instead of every site it
    /// asks for; nil = every site it asks for.
    var onlySites: [String]?
  }

  /// An extension downloaded and read, waiting for the user to add it.
  struct Pending: Identifiable {
    let id = UUID()
    let source: ExtensionSource
    /// Its unpacked folder.
    let folder: URL
    let webExtension: AnyObject
  }

  private(set) var records: [Record] = []
  /// Bumped when an action's icon, badge or title changes.
  private(set) var revision = 0
  /// The extension waiting for "Add" (the install sheet shows it).
  var pending: Pending?
  /// A store page being fetched.
  private(set) var fetching = false
  /// Why the last install failed.
  var installError: String?

  @ObservationIgnored weak var app: AppModel?
  @ObservationIgnored private let directory: URL
  @ObservationIgnored private var controllerBox: AnyObject?
  /// WKWebExtension and WKWebExtensionContext by record ID.
  @ObservationIgnored private var extensionsByID: [String: AnyObject] = [:]
  @ObservationIgnored private var contextsByID: [String: AnyObject] = [:]
  /// The adapters handed to WebKit, kept so each tab and window stays the
  /// same object (WebKit compares them by identity).
  @ObservationIgnored private var tabAdapters: [String: AnyObject] = [:]
  @ObservationIgnored private var windowAdapters: [String: AnyObject] = [:]
  /// A tiny page of each running extension in the app's window (keepAlive).
  @ObservationIgnored private var keepAliveViews: [String: WKWebView] = [:]

  static var isSupported: Bool {
    if #available(iOS 18.4, *) { true } else { false }
  }

  init(directory: URL) {
    self.directory = directory.appendingPathComponent("Extensions", isDirectory: true)
    super.init()
    try? FileManager.default.createDirectory(at: self.directory, withIntermediateDirectories: true)
    // Leftovers of installs that weren't confirmed.
    for file in (try? FileManager.default.contentsOfDirectory(at: self.directory, includingPropertiesForKeys: nil)) ?? []
    where file.lastPathComponent.hasPrefix("pending-") {
      try? FileManager.default.removeItem(at: file)
    }
    if let data = try? Data(contentsOf: indexURL), let records = try? JSONDecoder().decode([Record].self, from: data) {
      self.records = records
    }
    guard #available(iOS 18.4, *) else { return }
    let controller = WKWebExtensionController(configuration: .default())
    controller.delegate = self
    controllerBox = controller
    Task {
      for record in records { await load(record) }
      await checkForUpdates()
    }
  }

  private var indexURL: URL { directory.appendingPathComponent("Extensions.json") }
  private func folderURL(_ id: String) -> URL { directory.appendingPathComponent(id, isDirectory: true) }

  private func save() {
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
    try? encoder.encode(records).write(to: indexURL, options: .atomic)
  }

  // MARK: Tabs

  /// Attaches a new tab's configuration to the controller, so the
  /// extensions reach its pages. A configuration for an extension's own
  /// page (webkit-extension://) is that extension's.
  func configure(_ configuration: WKWebViewConfiguration, for url: URL?) -> WKWebViewConfiguration {
    guard #available(iOS 18.4, *), let controller else { return configuration }
    if let url, url.scheme == "webkit-extension", let context = controller.extensionContext(for: url),
      let own = context.webViewConfiguration
    {
      own.websiteDataStore = configuration.websiteDataStore
      ExtensionUnlock.shared.attach(to: own, extensionID: context.uniqueIdentifier, name: Self.name(of: context))
      return own
    }
    configuration.webExtensionController = controller
    return configuration
  }

  /// The extensions' context menu items for a long press in a tab.
  func menuItems(forTab id: String) -> [UIMenuElement] {
    guard #available(iOS 18.4, *), controller != nil else { return [] }
    let adapter = tab(id)
    return records.compactMap { context($0.id) }.flatMap { context -> [UIMenuElement] in
      let items = context.menuItems(for: adapter)
      guard items.count > 1 else { return items }
      // Several from one extension go under its name, as on the desktop.
      return [UIMenu(title: context.webExtension.displayShortName ?? context.webExtension.displayName ?? "",
                     image: context.webExtension.icon(for: CGSize(width: 18, height: 18)), children: items)]
    }
  }

  /// A tab's page changed (URL, title or loading).
  func tabChanged(_ id: String, url: Bool = false, title: Bool = false, loading: Bool = false) {
    guard #available(iOS 18.4, *), let controller, !controller.extensionContexts.isEmpty else { return }
    var changed: WKWebExtension.TabChangedProperties = []
    if url { changed.insert(.URL) }
    if title { changed.insert(.title) }
    if loading { changed.insert(.loading) }
    controller.didChangeTabProperties(changed, for: tab(id))
  }

  /// Tabs WebKit was told about (didOpenTab), once each.
  @ObservationIgnored private var announcedTabs: Set<String> = []

  func tabOpened(_ id: String) {
    guard #available(iOS 18.4, *), let controller, !controller.extensionContexts.isEmpty,
      !announcedTabs.contains(id)
    else { return }
    announcedTabs.insert(id)
    controller.didOpenTab(tab(id))
  }

  /// Tabs that opened before any extension was loaded (at launch, the
  /// restored ones) are announced once one is, and the shown one activated.
  private func announceLiveTabs() {
    guard #available(iOS 18.4, *), let controller, let app else { return }
    for id in app.tabs.live.keys.sorted() { tabOpened(id) }
    if let shown = app.shownTab, app.tabs.live[shown.id] != nil {
      controller.didActivateTab(tab(shown.id), previousActiveTab: nil)
    }
  }

  func tabClosed(_ id: String) {
    announcedTabs.remove(id)
    guard #available(iOS 18.4, *), let controller else { return }
    if let adapter = tabAdapters.removeValue(forKey: id) as? ExtensionTab, !controller.extensionContexts.isEmpty {
      controller.didCloseTab(adapter, windowIsClosing: false)
    }
  }

  func tabActivated(_ id: String, previous: String?) {
    guard #available(iOS 18.4, *), let controller, !controller.extensionContexts.isEmpty else { return }
    controller.didActivateTab(tab(id), previousActiveTab: previous.map(tab))
  }

  func spaceFocused(_ id: String) {
    guard #available(iOS 18.4, *), let controller, !controller.extensionContexts.isEmpty else { return }
    controller.didFocusWindow(window(id))
  }

  // MARK: Install

  /// Downloads a store's extension and asks the user to add it.
  func fetch(_ source: ExtensionSource) async {
    installError = nil
    guard Self.isSupported else {
      installError = ExtensionError.unsupported.localizedDescription
      return
    }
    fetching = true
    defer { fetching = false }
    do {
      try await prepare(zip: try await source.fetchZip(), source: source)
    } catch {
      installError = error.localizedDescription
    }
  }

  /// A package picked in Files (.crx, .xpi or .zip).
  func fetch(file: URL) async {
    installError = nil
    guard Self.isSupported else {
      installError = ExtensionError.unsupported.localizedDescription
      return
    }
    let scoped = file.startAccessingSecurityScopedResource()
    defer { if scoped { file.stopAccessingSecurityScopedResource() } }
    do {
      try await prepare(zip: try ExtensionSource.zip(fromCRX: try Data(contentsOf: file)), source: .file)
    } catch {
      installError = error.localizedDescription
    }
  }

  private func prepare(zip: Data, source: ExtensionSource) async throws {
    let pending = try await unpack(zip: zip, source: source)
    cancelPending()
    self.pending = pending
    if #available(iOS 18.4, *) { presentInstallSheet(pending) }
  }

  /// A package unpacked into a pending folder and read by WebKit.
  private func unpack(zip: Data, source: ExtensionSource) async throws -> Pending {
    guard #available(iOS 18.4, *) else { throw ExtensionError.unsupported }
    let url = directory.appendingPathComponent("pending-\(UUID().uuidString)", isDirectory: true)
    try ExtensionPackage.unpack(zip, to: url)
    do {
      return Pending(source: source, folder: url, webExtension: try await WKWebExtension(resourceBaseURL: url))
    } catch {
      try? FileManager.default.removeItem(at: url)
      throw error
    }
  }

  @available(iOS 18.4, *)
  private func presentInstallSheet(_ pending: Pending) {
    weak var host: UIViewController?
    let sheet = UIHostingController(
      rootView: ExtensionInstallSheet(extensions: self, pending: pending) { host?.dismiss(animated: true) })
    host = sheet
    // Add or Cancel: no swiping it away with the download left pending.
    sheet.isModalInPresentation = true
    sheet.sheetPresentationController?.detents = [.medium(), .large()]
    // At launch (a debug add, a file opened into the app) there may be no
    // window yet: try for a few seconds.
    Task {
      for _ in 0..<20 {
        // Added or cancelled meanwhile (a debug add): nothing to show.
        guard self.pending?.id == pending.id else { return }
        if Presenter.present(sheet) { return }
        try? await Task.sleep(for: .milliseconds(250))
      }
      cancelPending()
      installError = "Couldn't show the extension. Try again."
    }
  }

  func cancelPending() {
    if let pending { try? FileManager.default.removeItem(at: pending.folder) }
    pending = nil
  }

  /// "Add": the pending extension is kept and runs. Adding one that's
  /// installed (same store ID) updates it, keeping its data and settings.
  func confirmPending() async {
    guard let pending else { return }
    self.pending = nil
    await install(pending, keepState: false)
  }

  /// Puts a pending package in place and runs it. An update keeps the
  /// extension's on/off state, data and grants.
  private func install(_ pending: Pending, keepState: Bool) async {
    var record = records.first { $0.source == pending.source && pending.source != .file }
      ?? Record(id: pending.source.preferredIdentifier, source: pending.source)
    record.installed = Date()
    record.availableUpdate = nil
    if !keepState { record.enabled = true }
    unload(record.id)
    extensionsByID[record.id] = nil
    let destination = folderURL(record.id)
    try? FileManager.default.removeItem(at: destination)
    do {
      try FileManager.default.moveItem(at: pending.folder, to: destination)
    } catch {
      installError = error.localizedDescription
      return
    }
    if let index = records.firstIndex(where: { $0.id == record.id }) {
      records[index] = record
    } else {
      records.append(record)
    }
    save()
    await load(record)
  }

  func setEnabled(_ id: String, _ enabled: Bool) {
    guard let index = records.firstIndex(where: { $0.id == id }) else { return }
    records[index].enabled = enabled
    save()
    if enabled {
      Task { await load(records[index]) }
    } else {
      unload(id)
      // Keeps showing in the list.
      revision += 1
    }
  }

  /// Website access: every site it asks for (nil), or only `sites`. The
  /// extension restarts with the new grants.
  func setOnlySites(_ id: String, _ sites: [String]?) {
    guard let index = records.firstIndex(where: { $0.id == id }) else { return }
    records[index].onlySites = sites.map { Array(Set($0.map { $0.lowercased() })).sorted() }
    save()
    let record = records[index]
    unload(id)
    if record.enabled { Task { await load(record) } }
  }

  /// "Allow on <site>" in the page menu: adds or removes one site, moving
  /// the extension to "only these sites" if needed.
  func setAllowed(_ id: String, site: String, _ allowed: Bool) {
    guard let record = records.first(where: { $0.id == id }) else { return }
    var sites = record.onlySites ?? []
    if allowed { sites.append(site) } else { sites.removeAll { $0 == site } }
    setOnlySites(id, sites)
  }

  func remove(_ id: String) {
    unload(id)
    if #available(iOS 18.4, *), let controller {
      // Its storage goes with it.
      Task {
        let records = await controller.dataRecords(ofTypes: WKWebExtensionController.allExtensionDataTypes)
        let mine = records.filter { $0.uniqueIdentifier == id }
        if !mine.isEmpty {
          await controller.removeData(ofTypes: WKWebExtensionController.allExtensionDataTypes, from: mine)
        }
      }
    }
    extensionsByID[id] = nil
    records.removeAll { $0.id == id }
    try? FileManager.default.removeItem(at: folderURL(id))
    save()
  }

  /// Fetches the store's current version again, through the install
  /// sheet (the detail screen's Update and Review Update).
  func update(_ id: String) async {
    guard let record = records.first(where: { $0.id == id }) else { return }
    await fetch(record.source)
  }

  /// At most once a day (at launch): each store extension's current
  /// version. A newer one that asks for nothing more is installed quietly;
  /// one that asks for more waits for the user (`availableUpdate`).
  func checkForUpdates(force: Bool = false) async {
    guard #available(iOS 18.4, *) else { return }
    let key = "CompassExtensionsUpdateCheck"
    if !force, let last = UserDefaults.standard.object(forKey: key) as? Date, Date().timeIntervalSince(last) < 24 * 60 * 60 {
      return
    }
    UserDefaults.standard.set(Date(), forKey: key)
    for record in records where record.source != .file {
      guard let current = webExtension(record.id), let installed = current.version,
        let latest = try? await record.source.latestVersion(), ExtensionSource.isNewer(latest, than: installed),
        let zip = try? await record.source.fetchZip(),
        let pending = try? await unpack(zip: zip, source: record.source),
        let next = pending.webExtension as? WKWebExtension
      else { continue }
      if ExtensionPermissions.asksForMore(next, than: current) {
        try? FileManager.default.removeItem(at: pending.folder)
        if let index = records.firstIndex(where: { $0.id == record.id }) {
          records[index].availableUpdate = next.version
          save()
        }
      } else {
        await install(pending, keepState: true)
      }
    }
  }

  // MARK: Loading

  private func load(_ record: Record) async {
    guard #available(iOS 18.4, *), let controller else { return }
    let webExtension: WKWebExtension
    if let loaded = extensionsByID[record.id] as? WKWebExtension {
      webExtension = loaded
    } else {
      let made: WKWebExtension
      ExtensionPackage.refresh(in: folderURL(record.id))
      do {
        made = try await WKWebExtension(resourceBaseURL: folderURL(record.id))
      } catch {
        #if DEBUG
          print("WebExtensions: \(record.id) can't be read: \(error)")
        #endif
        return
      }
      webExtension = made
      extensionsByID[record.id] = made
    }
    revision += 1
    guard record.enabled, contextsByID[record.id] == nil else { return }
    let context = WKWebExtensionContext(for: webExtension)
    context.uniqueIdentifier = record.id
    // The same origin every launch: WebKit picks a random one otherwise,
    // and the extension's IndexedDB and localStorage (1Password's
    // accounts) belong to the origin, so they'd be gone after a restart.
    if let base = URL(string: "webkit-extension://\(record.id.lowercased())/") { context.baseURL = base }
    context.isInspectable = true
    // What it asked for was shown, and accepted, when it was added.
    let optionalPatterns = webExtension.optionalPermissionMatchPatterns
    for permission in webExtension.requestedPermissions {
      context.setPermissionStatus(.grantedExplicitly, for: permission)
    }
    if let sites = record.onlySites {
      // Website access → Only on these sites.
      for site in sites {
        for string in ["*://\(site)/*", "*://*.\(site)/*"] {
          if let pattern = try? WKWebExtension.MatchPattern(string: string) {
            context.setPermissionStatus(.grantedExplicitly, for: pattern)
          }
        }
      }
    } else {
      for pattern in webExtension.allRequestedMatchPatterns where !optionalPatterns.contains(pattern) {
        context.setPermissionStatus(.grantedExplicitly, for: pattern)
      }
    }
    for name in record.grantedOptional {
      context.setPermissionStatus(.grantedExplicitly, for: WKWebExtension.Permission(rawValue: name))
    }
    for string in record.grantedOptionalPatterns {
      if let pattern = try? WKWebExtension.MatchPattern(string: string) {
        context.setPermissionStatus(.grantedExplicitly, for: pattern)
      }
    }
    do {
      try controller.load(context)
      contextsByID[record.id] = context
      announceLiveTabs()
      keepAlive(context, id: record.id)
      // Started now rather than on its first event: extensions set their
      // badges and state up as they start.
      context.loadBackgroundContent { error in
        #if DEBUG
          print("WebExtensions: \(record.id) background:", error.map { "\($0)" } ?? "started")
        #endif
      }
      // Pages that loaded before it started (at launch, or before it was
      // added) get its content scripts with a reload.
      for view in app?.tabs.live.values ?? [:].values {
        if let url = view.url, context.hasInjectedContent(for: url) { view.reload() }
      }
    } catch {
      installError = "\(webExtension.displayName ?? "The extension") couldn't start: \(error.localizedDescription)"
    }
    #if DEBUG
      print("WebExtensions: loaded \(record.id) \(webExtension.displayName ?? "?") \(webExtension.version ?? "")",
            "errors:", (webExtension.errors + context.errors).map(\.localizedDescription))
    #endif
    revision += 1
  }

  private func unload(_ id: String) {
    keepAliveViews.removeValue(forKey: id)?.removeFromSuperview()
    guard #available(iOS 18.4, *), let controller,
      let context = contextsByID.removeValue(forKey: id) as? WKWebExtensionContext
    else { return }
    try? controller.unload(context)
    revision += 1
  }
}

// MARK: - iOS 18.4

@available(iOS 18.4, *)
extension WebExtensions {
  var controller: WKWebExtensionController? { controllerBox as? WKWebExtensionController }

  func webExtension(_ id: String) -> WKWebExtension? { extensionsByID[id] as? WKWebExtension }

  /// "1Password", not "1Password – Password Manager".
  static func name(of context: WKWebExtensionContext) -> String {
    let full = context.webExtension.displayShortName ?? context.webExtension.displayName ?? "the extension"
    return String(full.split(separator: " – ").first ?? Substring(full))
  }
  func context(_ id: String) -> WKWebExtensionContext? { contextsByID[id] as? WKWebExtensionContext }

  /// The running extensions with an action (a toolbar button), in the
  /// order they were added, for the page menu.
  func actions(forTab id: String?) -> [(record: Record, action: WKWebExtension.Action)] {
    _ = revision
    return records.compactMap { record in
      guard let context = context(record.id), let action = context.action(for: id.map(tab)) else { return nil }
      return (record, action)
    }
  }

  /// action.openPopup() from the extension (ExtensionPackage's polyfill):
  /// its popup over the shown tab, as a tap on its button would. Only when
  /// it has one, so it can't turn into a click event.
  func openPopup(_ context: WKWebExtensionContext) {
    let tabID = app?.shownTab?.id
    guard context.action(for: tabID.map(tab))?.presentsPopup == true else { return }
    context.performAction(for: tabID.map(tab))
  }

  /// The menu's tap on an action: its popup, or its click event.
  func performAction(_ id: String, tab tabID: String?) {
    guard let context = context(id) else { return }
    context.performAction(for: tabID.map(tab))
  }

  func openOptions(_ id: String) {
    guard let context = context(id), let url = context.optionsPageURL else { return }
    presentPage(url, context: context)
  }

  func tab(_ id: String) -> ExtensionTab {
    if let adapter = tabAdapters[id] as? ExtensionTab { return adapter }
    let adapter = ExtensionTab(id: id, owner: self)
    tabAdapters[id] = adapter
    return adapter
  }

  func window(_ spaceID: String) -> ExtensionWindow {
    if let adapter = windowAdapters[spaceID] as? ExtensionWindow { return adapter }
    let adapter = ExtensionWindow(spaceID: spaceID, owner: self)
    windowAdapters[spaceID] = adapter
    return adapter
  }

  /// The space a tab entry counts as in: its own, or for a favorite, the
  /// shown space when it has the favorite's profile, else the profile's
  /// first space.
  func spaceID(of node: SidebarNode) -> String? {
    guard let app else { return nil }
    if let space = app.store.space(of: node) { return space.id }
    let profile = app.store.profile(of: node)
    if let shown = app.selectedSpace, app.store.sameProfile(shown.profileUUID, profile) { return shown.id }
    return app.store.spaces.first { app.store.sameProfile($0.profileUUID, profile) }?.id
  }

  /// A space's tabs as extensions see them: favorites (in the shown
  /// space), pinned, then Today.
  func tabIDs(inSpace spaceID: String) -> [String] {
    guard let app, let space = app.store.space(id: spaceID) else { return [] }
    var roots = [space.pinned, space.today]
    let favorites = app.store.favorites(for: space)
    if favorites.children.contains(where: { self.spaceID(of: $0) == spaceID }) { roots.insert(favorites, at: 0) }
    return roots.flatMap { $0.children.flatMap(\.subtree) }.filter(\.isTab).map(\.id)
  }

  /// iOS suspends a web process with nothing on screen, and a suspended
  /// process loses its IndexedDB connections: an extension's background
  /// page (1Password's) finds its database closed a few seconds after it
  /// starts, and restarts on its error page. An extension's pages share
  /// its background's process, so one of them, 1 pt and nearly
  /// transparent in the app's window, keeps that process awake while
  /// Compass is in front (docs/design/ios.md, "Web extensions").
  func keepAlive(_ context: WKWebExtensionContext, id: String) {
    guard keepAliveViews[id] == nil, context.webExtension.hasBackgroundContent,
      let configuration = context.webViewConfiguration
    else { return }
    let view = WKWebView(frame: CGRect(x: 0, y: 0, width: 1, height: 1), configuration: configuration)
    view.alpha = 0.011
    view.isUserInteractionEnabled = false
    view.accessibilityElementsHidden = true
    view.load(URLRequest(url: context.baseURL.appendingPathComponent(ExtensionPackage.keepAliveName)))
    keepAliveViews[id] = view
    Task { @MainActor in
      // The window may not be up yet at launch.
      for _ in 0..<40 {
        let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
        if let window = scene?.keyWindow ?? scene?.windows.first {
          window.insertSubview(view, at: 0)
          return
        }
        try? await Task.sleep(for: .milliseconds(250))
      }
    }
  }

  /// An extension's page (options) in a sheet, in its own web view.
  func presentPage(_ url: URL, context: WKWebExtensionContext) {
    guard let configuration = context.webViewConfiguration else { return }
    ExtensionUnlock.shared.attach(to: configuration, extensionID: context.uniqueIdentifier, name: Self.name(of: context))
    let view = WKWebView(frame: .zero, configuration: configuration)
    view.isInspectable = true
    view.load(URLRequest(url: url))
    let page = UIViewController()
    page.view = view
    page.title = context.webExtension.displayName
    page.navigationItem.rightBarButtonItem = UIBarButtonItem(
      systemItem: .done, primaryAction: UIAction { [weak page] _ in page?.dismiss(animated: true) })
    Presenter.present(UINavigationController(rootViewController: page))
  }

  /// "Allow" or "Don't Allow", for what an extension asks for while it runs.
  fileprivate func ask(_ context: WKWebExtensionContext, _ what: String) async -> Bool {
    // WebKit waits on the answer: it must come exactly once, and "no" when
    // the alert couldn't be shown (UIKit drops a presentation over one
    // that's still animating).
    final class Once: @unchecked Sendable { var done = false }
    let once = Once()
    return await withCheckedContinuation { continuation in
      let answer: @MainActor (Bool) -> Void = { value in
        guard !once.done else { return }
        once.done = true
        continuation.resume(returning: value)
      }
      let name = context.webExtension.displayName ?? "An extension"
      let alert = UIAlertController(title: "\(name) wants to \(what)", message: nil, preferredStyle: .alert)
      alert.addAction(UIAlertAction(title: "Don't Allow", style: .cancel) { _ in answer(false) })
      alert.addAction(UIAlertAction(title: "Allow", style: .default) { _ in answer(true) })
      guard Presenter.present(alert) else { return answer(false) }
      Task { @MainActor in
        try? await Task.sleep(for: .seconds(2))
        if alert.presentingViewController == nil { answer(false) }
      }
    }
  }

  fileprivate func record(of context: WKWebExtensionContext) -> Int? {
    records.firstIndex { $0.id == context.uniqueIdentifier }
  }

  fileprivate func remember(_ context: WKWebExtensionContext, permissions: [String] = [], patterns: [String] = []) {
    guard let index = record(of: context) else { return }
    records[index].grantedOptional = Array(Set(records[index].grantedOptional + permissions)).sorted()
    records[index].grantedOptionalPatterns = Array(Set(records[index].grantedOptionalPatterns + patterns)).sorted()
    save()
  }

  /// Opens a tab for an extension: a Today entry in its window's space (or
  /// the shown one).
  fileprivate func openTab(url: URL?, spaceID: String?, active: Bool, pinned: Bool) -> ExtensionTab? {
    guard let app, let space = spaceID.flatMap(app.store.space(id:)) ?? app.selectedSpace,
      let node = app.store.addTab(url: url?.absoluteString ?? "", to: pinned ? space.pinned : space.today)
    else { return nil }
    if active { app.show(node) }
    tabOpened(node.id)
    return tab(node.id)
  }
}

@available(iOS 18.4, *)
extension WebExtensions: WKWebExtensionControllerDelegate {
  func webExtensionController(
    _ controller: WKWebExtensionController, openWindowsFor extensionContext: WKWebExtensionContext
  ) -> [any WKWebExtensionWindow] {
    guard let app else { return [] }
    // The focused one first.
    let selected = app.selectedSpace?.id
    let ids = app.store.spaces.map(\.id)
    let ordered = ids.filter { $0 == selected } + ids.filter { $0 != selected }
    return ordered.map(window)
  }

  func webExtensionController(
    _ controller: WKWebExtensionController, focusedWindowFor extensionContext: WKWebExtensionContext
  ) -> (any WKWebExtensionWindow)? {
    app?.selectedSpace.map { window($0.id) }
  }

  func webExtensionController(
    _ controller: WKWebExtensionController, openNewTabUsing configuration: WKWebExtension.TabConfiguration,
    for extensionContext: WKWebExtensionContext, completionHandler: @escaping ((any WKWebExtensionTab)?, (any Error)?) -> Void
  ) {
    let spaceID = (configuration.window as? ExtensionWindow)?.spaceID
    #if DEBUG
      print("WebExtensions: open tab", configuration.url?.absoluteString ?? "")
    #endif
    let tab = openTab(url: configuration.url, spaceID: spaceID, active: configuration.shouldBeActive,
                      pinned: configuration.shouldBePinned)
    completionHandler(tab, nil)
  }

  /// The phone has no windows to open: the tabs open in the shown space.
  func webExtensionController(
    _ controller: WKWebExtensionController, openNewWindowUsing configuration: WKWebExtension.WindowConfiguration,
    for extensionContext: WKWebExtensionContext,
    completionHandler: @escaping ((any WKWebExtensionWindow)?, (any Error)?) -> Void
  ) {
    let urls = configuration.tabURLs
    for (index, url) in urls.enumerated() {
      _ = openTab(url: url, spaceID: nil, active: index == urls.count - 1, pinned: false)
    }
    completionHandler(app?.selectedSpace.map { window($0.id) }, nil)
  }

  func webExtensionController(
    _ controller: WKWebExtensionController, openOptionsPageFor extensionContext: WKWebExtensionContext,
    completionHandler: @escaping ((any Error)?) -> Void
  ) {
    if let url = extensionContext.optionsPageURL { presentPage(url, context: extensionContext) }
    completionHandler(nil)
  }

  func webExtensionController(
    _ controller: WKWebExtensionController, promptForPermissions permissions: Set<WKWebExtension.Permission>,
    in tab: (any WKWebExtensionTab)?, for extensionContext: WKWebExtensionContext,
    completionHandler: @escaping (Set<WKWebExtension.Permission>, Date?) -> Void
  ) {
    Task {
      let what = permissions.map { ExtensionPermissions.describe($0) }.sorted().joined(separator: ", ")
      guard await ask(extensionContext, what.isEmpty ? "get more permissions" : what) else {
        completionHandler([], nil)
        return
      }
      remember(extensionContext, permissions: permissions.map(\.rawValue))
      completionHandler(permissions, nil)
    }
  }

  /// WebKit asks this at the end of every tabs.query (for the matched
  /// tabs' URLs), so it isn't a prompt: Chrome never asks there. Nothing new
  /// is granted; WebKit leaves out the URLs and titles of tabs the
  /// extension can't reach (docs/design/ios.md, "Web extensions").
  func webExtensionController(
    _ controller: WKWebExtensionController, promptForPermissionToAccess urls: Set<URL>,
    in tab: (any WKWebExtensionTab)?, for extensionContext: WKWebExtensionContext,
    completionHandler: @escaping (Set<URL>, Date?) -> Void
  ) {
    completionHandler(urls.filter { extensionContext.hasAccess(to: $0) }, nil)
  }

  func webExtensionController(
    _ controller: WKWebExtensionController,
    promptForPermissionMatchPatterns matchPatterns: Set<WKWebExtension.MatchPattern>,
    in tab: (any WKWebExtensionTab)?, for extensionContext: WKWebExtensionContext,
    completionHandler: @escaping (Set<WKWebExtension.MatchPattern>, Date?) -> Void
  ) {
    Task {
      guard await ask(extensionContext, ExtensionPermissions.describe(matchPatterns)) else {
        completionHandler([], nil)
        return
      }
      remember(extensionContext, patterns: matchPatterns.map(\.string))
      completionHandler(matchPatterns, nil)
    }
  }

  func webExtensionController(
    _ controller: WKWebExtensionController, didUpdate action: WKWebExtension.Action,
    forExtensionContext context: WKWebExtensionContext
  ) {
    revision += 1
  }

  /// Native messaging: there are no native apps to talk to here (1Password
  /// asks for its desktop app, then works on its own). Debug builds print
  /// what the debug script sends (ExtensionPackage.addDebugScript).
  func webExtensionController(
    _ controller: WKWebExtensionController, sendMessage message: Any, toApplicationWithIdentifier applicationIdentifier: String?,
    for extensionContext: WKWebExtensionContext, replyHandler: @escaping (Any?, (any Error)?) -> Void
  ) {
    if applicationIdentifier == ExtensionPackage.openPopupApplication {
      openPopup(extensionContext)
      return replyHandler(nil, nil)
    }
    #if DEBUG
      if applicationIdentifier == ExtensionPackage.debugApplication, let body = message as? [String: Any] {
        print("WebExtensions: [\(extensionContext.webExtension.displayShortName ?? "?")] \(body["level"] ?? "") \(body["where"] ?? ""): \(body["text"] ?? "")")
        return replyHandler(nil, nil)
      }
    #endif
    replyHandler(nil, ExtensionError.noNativeApp)
  }

  func webExtensionController(
    _ controller: WKWebExtensionController, connectUsing port: WKWebExtension.MessagePort,
    for extensionContext: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void
  ) {
    completionHandler(ExtensionError.noNativeApp)
  }

  /// The action's popup, as a sheet over whatever is up.
  func webExtensionController(
    _ controller: WKWebExtensionController, presentActionPopup action: WKWebExtension.Action,
    for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void
  ) {
    guard let popup = action.popupViewController else {
      completionHandler(nil)
      return
    }
    popup.modalPresentationStyle = .pageSheet
    if let sheet = popup.sheetPresentationController {
      sheet.detents = [.medium(), .large()]
      sheet.prefersGrabberVisible = true
    }
    // WebKit draws its title over the page in the system's style, which is
    // unreadable over a dark popup (and changing the controller's style
    // hangs it): no title; the popup names itself.
    popup.title = ""
    popup.navigationItem.title = ""
    Presenter.present(popup)
    if let view = action.popupWebView {
      ExtensionUnlock.shared.attach(to: view, extensionID: context.uniqueIdentifier, name: Self.name(of: context))
      #if DEBUG
        //   -CompassPopupJS <js>: run in the popup 1.5 s after it's up.
        if let js = UserDefaults.standard.string(forKey: "CompassPopupJS") {
          Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(1500))
            do { _ = try await view.evaluateJavaScript(js) } catch { print("WebExtensions: popup JS:", error) }
          }
        }
      #endif
    }
    completionHandler(nil)
  }
}

/// Presents UIKit view controllers over whatever is up (sheets included).
@MainActor
enum Presenter {
  /// False when there's nothing to present it over (it isn't shown).
  @discardableResult
  static func present(_ controller: UIViewController) -> Bool {
    let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
      .first { $0.activationState == .foregroundActive } ?? UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
    guard var top = (scene?.keyWindow ?? scene?.windows.first)?.rootViewController else { return false }
    while let presented = top.presentedViewController, !presented.isBeingDismissed { top = presented }
    top.present(controller, animated: true)
    return true
  }
}

extension Presenter {
  /// Dismisses whatever UIKit presented last (debug runs).
  static func dismissTop() {
    let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
    guard var top = scene?.keyWindow?.rootViewController else { return }
    while let presented = top.presentedViewController { top = presented }
    top.presentingViewController?.dismiss(animated: false)
  }
}
