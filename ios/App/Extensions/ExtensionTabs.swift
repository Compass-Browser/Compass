// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import UIKit
import WebKit

/// A sidebar tab entry, to extensions. It holds only the entry's ID, so it
/// follows the entry wherever it moves; a closed entry answers as an empty,
/// unloaded tab until WebKit lets go of it.
@available(iOS 18.4, *)
@MainActor
final class ExtensionTab: NSObject, WKWebExtensionTab {
  let id: String
  private weak var owner: WebExtensions?

  init(id: String, owner: WebExtensions) {
    self.id = id
    self.owner = owner
  }

  private var app: AppModel? { owner?.app }

  private var node: SidebarNode? { app?.store.node(id: id) }
  private var view: WKWebView? { app?.tabs.live[id] }

  func window(for context: WKWebExtensionContext) -> (any WKWebExtensionWindow)? {
    guard let owner, let node, let space = owner.spaceID(of: node) else { return nil }
    return owner.window(space)
  }

  func indexInWindow(for context: WKWebExtensionContext) -> Int {
    guard let owner, let node, let space = owner.spaceID(of: node) else { return 0 }
    return owner.tabIDs(inSpace: space).firstIndex(of: id) ?? 0
  }

  func webView(for context: WKWebExtensionContext) -> WKWebView? { view }

  func title(for context: WKWebExtensionContext) -> String? { view?.title ?? node?.displayTitle }

  func url(for context: WKWebExtensionContext) -> URL? { view?.url ?? node.flatMap { URL(string: $0.url) } }

  func isPinned(for context: WKWebExtensionContext) -> Bool { node?.section == .pinned }

  func isSelected(for context: WKWebExtensionContext) -> Bool { app?.shownTab?.id == id }

  func isLoadingComplete(for context: WKWebExtensionContext) -> Bool { view?.isLoading != true }

  func size(for context: WKWebExtensionContext) -> CGSize { view?.bounds.size ?? UIScreen.main.bounds.size }

  func zoomFactor(for context: WKWebExtensionContext) -> Double { Double(view?.pageZoom ?? 1) }

  func setZoomFactor(_ zoomFactor: Double, for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void) {
    view?.pageZoom = zoomFactor
    completionHandler(nil)
  }

  func loadURL(_ url: URL, for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void) {
    guard let app, let node else { return completionHandler(nil) }
    app.store.update(node, url: url.absoluteString)
    app.tabs.webView(for: node).load(URLRequest(url: url))
    completionHandler(nil)
  }

  func reload(fromOrigin: Bool, for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void) {
    if fromOrigin { view?.reloadFromOrigin() } else { view?.reload() }
    completionHandler(nil)
  }

  func goBack(for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void) {
    view?.goBack()
    completionHandler(nil)
  }

  func goForward(for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void) {
    view?.goForward()
    completionHandler(nil)
  }

  func activate(for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void) {
    if let app, let node { app.show(node) }
    completionHandler(nil)
  }

  func setSelected(_ selected: Bool, for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void) {
    if selected, let app, let node { app.show(node) }
    completionHandler(nil)
  }

  func setPinned(_ pinned: Bool, for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void) {
    guard let app, let node, let space = app.store.space(of: node),
      node.section == .today || node.section == .pinned
    else { return completionHandler(nil) }
    let target = pinned ? space.pinned : space.today
    if node.parent !== target { _ = app.store.move(node, to: target, at: target.children.count) }
    completionHandler(nil)
  }

  /// As the menu's close: a Today entry goes to the archive, anything else
  /// (pinned, a favorite) only unloads.
  func close(for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void) {
    guard let app, let node else { return completionHandler(nil) }
    if app.selectedNodeID == node.id { app.selectedNodeID = nil }
    if node.section == .today, node.parent?.isSplit != true { app.store.archive(node) }
    app.tabs.unload(id)
    owner?.tabClosed(id)
    completionHandler(nil)
  }

  func duplicate(
    using configuration: WKWebExtension.TabConfiguration, for context: WKWebExtensionContext,
    completionHandler: @escaping ((any WKWebExtensionTab)?, (any Error)?) -> Void
  ) {
    guard let app, let node, let space = owner.flatMap({ $0.spaceID(of: node) }).flatMap(app.store.space(id:)),
      let copy = app.store.addTab(url: url(for: context)?.absoluteString ?? node.url, to: space.today)
    else { return completionHandler(nil, nil) }
    if configuration.shouldBeActive { app.show(copy) }
    owner?.tabOpened(copy.id)
    completionHandler(owner?.tab(copy.id), nil)
  }

  func takeSnapshot(
    using configuration: WKSnapshotConfiguration, for context: WKWebExtensionContext,
    completionHandler: @escaping (UIImage?, (any Error)?) -> Void
  ) {
    guard let view else { return completionHandler(nil, nil) }
    view.takeSnapshot(with: configuration) { image, error in completionHandler(image, error) }
  }
}

/// A space, to extensions: a window of its tabs. The shown space is the
/// focused window, always full screen.
@available(iOS 18.4, *)
@MainActor
final class ExtensionWindow: NSObject, WKWebExtensionWindow {
  let spaceID: String
  private weak var owner: WebExtensions?

  init(spaceID: String, owner: WebExtensions) {
    self.spaceID = spaceID
    self.owner = owner
  }

  func tabs(for context: WKWebExtensionContext) -> [any WKWebExtensionTab] {
    guard let owner else { return [] }
    return owner.tabIDs(inSpace: spaceID).map(owner.tab)
  }

  func activeTab(for context: WKWebExtensionContext) -> (any WKWebExtensionTab)? {
    guard let owner, let app = owner.app, app.selectedSpace?.id == spaceID, let shown = app.shownTab else { return nil }
    return owner.tab(shown.id)
  }

  func windowType(for context: WKWebExtensionContext) -> WKWebExtension.WindowType { .normal }

  func windowState(for context: WKWebExtensionContext) -> WKWebExtension.WindowState { .fullscreen }

  func isPrivate(for context: WKWebExtensionContext) -> Bool { false }

  func screenFrame(for context: WKWebExtensionContext) -> CGRect { UIScreen.main.bounds }

  func frame(for context: WKWebExtensionContext) -> CGRect { UIScreen.main.bounds }

  func focus(for context: WKWebExtensionContext, completionHandler: @escaping ((any Error)?) -> Void) {
    if let app = owner?.app, app.store.space(id: spaceID) != nil { app.selectedSpaceID = spaceID }
    completionHandler(nil)
  }
}

/// What an extension's permissions mean, in words, for the install sheet
/// and the prompts.
@available(iOS 18.4, *)
@MainActor
enum ExtensionPermissions {
  static func describe(_ permission: WKWebExtension.Permission) -> String {
    switch permission {
    case .tabs, .webNavigation: "see the pages you visit"
    case .cookies: "read and change cookies"
    case .declarativeNetRequest, .declarativeNetRequestWithHostAccess: "block content on pages"
    case .declarativeNetRequestFeedback: "see what it blocked"
    case .clipboardWrite: "copy to the clipboard"
    case .nativeMessaging: "talk to other apps"
    case .scripting: "run scripts on pages it can reach"
    case .activeTab: "reach the page when you use it"
    case .contextMenus, .menus: "add to menus"
    case .alarms, .storage, .unlimitedStorage: "store data"
    default: permission.rawValue
    }
  }

  static func describe(_ patterns: Set<WKWebExtension.MatchPattern>) -> String {
    if patterns.contains(where: { $0.matchesAllHosts }) { return "read and change your data on all websites" }
    let hosts = Set(patterns.compactMap { $0.host }).sorted()
    if hosts.isEmpty { return "read and change data on some websites" }
    let shown = hosts.prefix(3).joined(separator: ", ")
    return "read and change your data on \(shown)" + (hosts.count > 3 ? " and \(hosts.count - 3) more" : "")
  }

  /// Whether `next` asks for permissions or sites `current` doesn't have.
  static func asksForMore(_ next: WKWebExtension, than current: WKWebExtension) -> Bool {
    if !next.requestedPermissions.isSubset(of: current.requestedPermissions) { return true }
    let had = current.allRequestedMatchPatterns
    if had.contains(where: { $0.matchesAllURLs }) { return false }
    return next.allRequestedMatchPatterns.contains { pattern in !had.contains { $0.matches(pattern) } }
  }

  /// The lines of the install sheet: site access first, then what else it
  /// can do; storage and the like go unsaid.
  static func summary(_ webExtension: WKWebExtension) -> [String] {
    var lines: [String] = []
    let optional = webExtension.optionalPermissionMatchPatterns
    let patterns = webExtension.allRequestedMatchPatterns.filter { !optional.contains($0) }
    if !patterns.isEmpty { lines.append(describe(patterns)) }
    let quiet: Set<WKWebExtension.Permission> = [.alarms, .storage, .unlimitedStorage, .contextMenus, .menus]
    lines += Set(webExtension.requestedPermissions.filter { !quiet.contains($0) }.map(describe)).sorted()
    return lines.map { $0.prefix(1).uppercased() + $0.dropFirst() }
  }
}
