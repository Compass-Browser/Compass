// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
import Foundation
import Observation

/// The app's state: the sidebar, what's selected, live tabs and sync.
@MainActor
@Observable
final class AppModel {
  let store = SpaceStore()
  let tabs: TabManager
  let sync: SyncController
  let passwords = PasswordManager()
  let siteSettings: SiteSettings
  let directory: URL

  /// The space the sidebar shows.
  var selectedSpaceID: String?
  /// The entry the browser shows; nil = an empty page.
  var selectedNodeID: String?
  /// The sidebar, a bottom sheet over the browser. Up at launch.
  var showingSidebar = true
  /// The sidebar rests at half height (dragged there) instead of full.
  var sidebarHalf = false
  /// While a finger moves the sidebar (a swipe up on the browser's bar, a
  /// drag on its top strip): where it is, 0 closed to 1 full. nil otherwise.
  var sidebarDrag: CGFloat?
  /// How far the sidebar's list is pulled down past its top, in points: the
  /// panel follows it down (SidebarPanel).
  var sidebarPull: CGFloat = 0
  /// For splits: which pane is in front.
  var selectedPane = 0
  var showingCommandBar = false
  /// The bottom bar's menu (PageMenu), and Site Settings from it.
  var showingMenu = false
  var showingSiteSettings = false
  /// The command bar opens a new tab (the "+" buttons), not the shown tab's
  /// address (the address pill).
  var commandBarNewTab = true
  var showingSettings = false
  /// After joining a chain: what came over (profiles and spaces).
  var showingJoinSummary = false
  /// A pairing link opened from the camera, for the join screen.
  var pendingInvite: PairingInvite?

  private var saveTask: Task<Void, Never>?
  private var archiveTimer: Task<Void, Never>?

  init() {
    directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Compass", isDirectory: true)
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let store = self.store
    try? store.load(from: directory.appendingPathComponent("Sidebar.json"))
    if store.spaces.isEmpty {
      store.applyingRemote { _ = store.addSpace(name: "Home") }
    }
    siteSettings = SiteSettings(directory: directory)
    tabs = TabManager(store: store, siteSettings: siteSettings)
    let history = (try? HistoryStore(path: directory.appendingPathComponent("History.sqlite").path)) ?? (try! HistoryStore())
    history.expire()
    sync = SyncController(store: store, history: history, passwords: passwords, directory: directory)
    selectedSpaceID = store.spaces.first?.id
    tabs.openInNewTab = { [weak self] url in self?.open(url.absoluteString) }
    tabs.onPageLoaded = { [weak self] url, title, id in
      guard let self else { return }
      self.sync.recordVisit(url: url, title: title, profile: self.accountProfile(ofTab: id))
    }
    tabs.onPasswordMessage = { [weak self] body, id, url in
      guard let self else { return }
      self.passwords.handle(body, tabID: id, pageURL: url, profile: self.accountProfile(ofTab: id))
    }
    tabs.onNavigated = { [weak self] id in self?.passwords.pageChanged(id) }
    Favicons.shared.onPageIcon = { [weak self] host, png, size, tab in
      guard let self else { return }
      self.sync.offerFavicon(host: host, png: png, sourceSize: size, account: self.accountProfile(ofTab: tab))
    }
    sync.isShown = { [weak self] id in self?.selectedNodeID == id }
    sync.onReplaced = { [weak self] old, new in
      guard let self else { return }
      self.tabs.rekey(old, to: new)
      if self.selectedNodeID == old { self.selectedNodeID = new }
    }
    #if DEBUG
      // Simulator checks (docs/design/ios.md, "Verification"):
      //   -CompassServer <url> -CompassJoinRecovery "<12 words>"
      let defaults = UserDefaults.standard
      if !sync.isOn, let words = defaults.string(forKey: "CompassJoinRecovery"),
        let server = defaults.string(forKey: "CompassServer").flatMap(URL.init(string:))
      {
        try? sync.join(recoveryCode: words, server: server)
        //   -CompassJoinSummary YES: the screen a join from Settings ends on.
        if defaults.bool(forKey: "CompassJoinSummary") {
          showingSettings = true
          showingJoinSummary = true
        }
      }
      //   -CompassOpenURL <url>: open a new tab.
      if let url = defaults.string(forKey: "CompassOpenURL") { open(url) }
      //   -CompassSidebarDrag <0...1>: hold the sidebar there, as under a
      //   finger.
      if let drag = defaults.object(forKey: "CompassSidebarDrag").flatMap({ Double("\($0)") }) {
        Task { [weak self] in
          try? await Task.sleep(for: .milliseconds(1500))
          self?.sidebarDrag = drag
        }
      }
      //   -CompassMenu YES: open the bottom bar's menu.
      if defaults.bool(forKey: "CompassMenu") {
        Task { [weak self] in
          try? await Task.sleep(for: .milliseconds(1500))
          self?.showingMenu = true
        }
      }
      //   -CompassCommandBar YES: open the command bar.
      if defaults.bool(forKey: "CompassCommandBar") {
        Task { [weak self] in
          try? await Task.sleep(for: .milliseconds(800))
          self?.openCommandBar(newTab: false)
        }
      }
      //   -CompassDebugJS <js>: run in each page once it has loaded.
      if let js = defaults.string(forKey: "CompassDebugJS") {
        let previous = tabs.onPageLoaded
        tabs.onPageLoaded = { [weak self] url, title, id in
          previous?(url, title, id)
          guard let self, let tab = self.shownTab, let view = self.tabs.live[tab.id] else { return }
          view.evaluateJavaScript(js, in: nil, in: .page, completionHandler: nil)
        }
      }
      //   -CompassSpace <name>: show that space once it's there.
      if let name = defaults.string(forKey: "CompassSpace") {
        Task { [weak self] in
          for _ in 0..<50 {
            if let self, let space = self.store.spaces.first(where: { $0.name == name }) {
              self.selectedSpaceID = space.id
              return
            }
            try? await Task.sleep(for: .milliseconds(200))
          }
        }
      }
      //   -CompassShow <title>: open that entry once it's there.
      if let title = defaults.string(forKey: "CompassShow") {
        Task { [weak self] in
          for _ in 0..<50 {
            if let self, let node = self.store.allNodes.first(where: { $0.displayTitle == title }) {
              self.show(node)
              return
            }
            try? await Task.sleep(for: .milliseconds(200))
          }
        }
      }
    #endif
    // Save after every change, local or synced (observation of the store).
    observeForSaving()
    archiveTimer = Task { [weak self] in
      while !Task.isCancelled {
        self?.store.archiveIdleEntries()
        self?.store.pruneArchive()
        try? await Task.sleep(for: .seconds(60))
      }
    }
  }

  var sidebarURL: URL { directory.appendingPathComponent("Sidebar.json") }

  /// The profile account a tab's history and passwords go to (its space's).
  func accountProfile(ofTab id: String) -> String {
    store.node(id: id).map(sync.accountProfile(of:)) ?? ""
  }

  private func observeForSaving() {
    withObservationTracking {
      _ = store.spaces.map { ($0.name, $0.color, $0.allNodes.map { ($0.url, $0.title, $0.children.count) }) }
      _ = store.favoritesRoots.map(\.children.count)
      _ = store.spaces.map(\.profileUUID)
    } onChange: { [weak self] in
      Task { @MainActor in
        self?.scheduleSave()
        self?.observeForSaving()
      }
    }
  }

  func scheduleSave() {
    saveTask?.cancel()
    saveTask = Task { [weak self] in
      try? await Task.sleep(for: .seconds(1))
      guard let self, !Task.isCancelled else { return }
      try? self.store.save(to: self.sidebarURL)
      self.tabs.prune()
    }
  }

  var selectedSpace: Space? {
    selectedSpaceID.flatMap(store.space(id:)) ?? store.spaces.first
  }

  var selectedNode: SidebarNode? { selectedNodeID.flatMap(store.node(id:)) }

  /// The tab shown for the selected entry (a split's front pane).
  var shownTab: SidebarNode? {
    guard let node = selectedNode else { return nil }
    if node.isSplit {
      let panes = node.children
      return panes.isEmpty ? nil : panes[min(selectedPane, panes.count - 1)]
    }
    return node.isTab ? node : nil
  }

  func show(_ node: SidebarNode) {
    if node.isFolder {
      store.setCollapsed(node, !node.collapsed)
      return
    }
    if let space = store.space(of: node) { selectedSpaceID = space.id }
    if let split = node.parent, split.isSplit {
      selectedNodeID = split.id
      selectedPane = split.children.firstIndex { $0 === node } ?? 0
    } else {
      selectedNodeID = node.id
      selectedPane = 0
    }
    store.touch(node)
    showingCommandBar = false
    showingSidebar = false
    sidebarHalf = false
  }

  /// Opens the find bar on the shown page, with `text` already searched.
  /// After a sheet has closed (it can't show while one is up).
  func findOnPage(_ text: String? = nil) {
    guard let tab = shownTab, let view = tabs.live[tab.id] else { return }
    Task {
      try? await Task.sleep(for: .milliseconds(450))
      view.findInteraction?.presentFindNavigator(showingReplace: false)
      if let text { view.findInteraction?.searchText = text }
    }
  }

  /// The command bar: for a new tab, or to edit the shown tab's address.
  func openCommandBar(newTab: Bool) {
    commandBarNewTab = newTab
    showingCommandBar = true
  }

  func showSidebar() {
    sidebarHalf = false
    showingSidebar = true
  }

  /// Opens text from the command bar: a URL, or a search.
  func open(_ text: String, replacing node: SidebarNode? = nil) {
    let url = Self.url(for: text)
    if let node, node.isTab {
      store.update(node, url: url)
      if let view = tabs.live[node.id], let u = URL(string: url) { view.load(URLRequest(url: u)) }
      show(node)
      return
    }
    guard let space = selectedSpace, let tab = store.addTab(url: url, to: space.today) else { return }
    show(tab)
  }

  static func url(for text: String) -> String {
    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    if let url = URL(string: trimmed), url.scheme != nil, url.host() != nil { return trimmed }
    if !trimmed.contains(" "), trimmed.contains("."), let url = URL(string: "https://" + trimmed), url.host() != nil {
      return url.absoluteString
    }
    var c = URLComponents(string: "https://duckduckgo.com/")!
    c.queryItems = [URLQueryItem(name: "q", value: trimmed)]
    return c.url!.absoluteString
  }

  func close(_ node: SidebarNode) {
    if selectedNodeID == node.id { selectedNodeID = nil }
    for tab in node.tabs { tabs.unload(tab.id) }
    store.remove(node)
  }
}
