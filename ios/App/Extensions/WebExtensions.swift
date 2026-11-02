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
      return own
    }
    configuration.webExtensionController = controller
    return configuration
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

  func tabOpened(_ id: String) {
    guard #available(iOS 18.4, *), let controller, !controller.extensionContexts.isEmpty else { return }
    controller.didOpenTab(tab(id))
  }

  func tabClosed(_ id: String) {
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
    guard #available(iOS 18.4, *) else { throw ExtensionError.unsupported }
    let url = directory.appendingPathComponent("pending-\(UUID().uuidString)", isDirectory: true)
    try ExtensionPackage.unpack(zip, to: url)
    do {
      let webExtension = try await WKWebExtension(resourceBaseURL: url)
      let pending = Pending(source: source, folder: url, webExtension: webExtension)
      cancelPending()
      self.pending = pending
      presentInstallSheet(pending)
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
    Presenter.present(sheet)
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
    var record = records.first { $0.source == pending.source && pending.source != .file }
      ?? Record(id: pending.source.preferredIdentifier, source: pending.source)
    record.installed = Date()
    record.enabled = true
    unload(record.id)
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

  /// Fetches the store's current version again.
  func update(_ id: String) async {
    guard let record = records.first(where: { $0.id == id }) else { return }
    await fetch(record.source)
  }

  // MARK: Loading

  private func load(_ record: Record) async {
    guard #available(iOS 18.4, *), let controller else { return }
    let webExtension: WKWebExtension
    if let loaded = extensionsByID[record.id] as? WKWebExtension {
      webExtension = loaded
    } else {
      let made: WKWebExtension
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
    context.isInspectable = true
    // What it asked for was shown, and accepted, when it was added.
    let optionalPatterns = webExtension.optionalPermissionMatchPatterns
    for permission in webExtension.requestedPermissions {
      context.setPermissionStatus(.grantedExplicitly, for: permission)
    }
    for pattern in webExtension.allRequestedMatchPatterns where !optionalPatterns.contains(pattern) {
      context.setPermissionStatus(.grantedExplicitly, for: pattern)
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

  /// An extension's page (options) in a sheet, in its own web view.
  func presentPage(_ url: URL, context: WKWebExtensionContext) {
    guard let configuration = context.webViewConfiguration else { return }
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
    await withCheckedContinuation { continuation in
      let name = context.webExtension.displayName ?? "An extension"
      let alert = UIAlertController(title: "\(name) wants to \(what)", message: nil, preferredStyle: .alert)
      alert.addAction(UIAlertAction(title: "Don't Allow", style: .cancel) { _ in continuation.resume(returning: false) })
      alert.addAction(UIAlertAction(title: "Allow", style: .default) { _ in continuation.resume(returning: true) })
      Presenter.present(alert)
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
    let ids = app.store.spaces.map(\.id).sorted { a, _ in a == selected }
    return ids.map(window)
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

  func webExtensionController(
    _ controller: WKWebExtensionController, promptForPermissionToAccess urls: Set<URL>,
    in tab: (any WKWebExtensionTab)?, for extensionContext: WKWebExtensionContext,
    completionHandler: @escaping (Set<URL>, Date?) -> Void
  ) {
    Task {
      let hosts = Set(urls.compactMap { $0.host() }).sorted().joined(separator: ", ")
      guard await ask(extensionContext, "read and change your data on \(hosts)") else {
        completionHandler([], nil)
        return
      }
      let patterns = Set(urls.compactMap { $0.host() }).map { "*://\($0)/*" }
      remember(extensionContext, patterns: patterns)
      completionHandler(urls, nil)
    }
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
    Presenter.present(popup)
    completionHandler(nil)
  }
}

/// Presents UIKit view controllers over whatever is up (sheets included).
@MainActor
enum Presenter {
  static func present(_ controller: UIViewController) {
    let scene = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first
    guard var top = scene?.keyWindow?.rootViewController else { return }
    while let presented = top.presentedViewController, !presented.isBeingDismissed { top = presented }
    top.present(controller, animated: true)
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
