// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
import CompassShared
import Foundation
import SwiftUI
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
  let contentBlocking = ContentBlocking()
  let downloads = Downloads()
  let extensions: WebExtensions
  let directory: URL
  /// Widgets, the share sheet, Spotlight, the Focus filter (App/System).
  let system: SystemBridge

  /// The space the sidebar shows (to extensions, the focused window).
  var selectedSpaceID: String? {
    didSet {
      if let id = selectedSpaceID, id != oldValue {
        extensions.spaceFocused(id)
        system.spaceShown(id)
      }
    }
  }
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
  /// The tab picker (TabPicker): the live tabs as cards. Opened by a swipe
  /// up on the bar's left end, which it follows: `pickerDrag` is how far
  /// open (0...1) under a finger, nil otherwise.
  var showingPicker = false
  var pickerDrag: CGFloat?
  /// The picker's cards (tab IDs, least recently shown first), fixed while
  /// it's open, and which one is centered (fractional mid-scroll).
  var pickerTabs: [String] = []
  var pickerPosition: CGFloat = 0
  /// The picker is closing onto a card: it stays up (the card full screen,
  /// over its page) until the switch underneath has drawn.
  var pickerHolding = false
  /// For splits: which pane is in front.
  var selectedPane = 0
  var showingCommandBar = false
  /// The bottom bar's menu (PageMenu), and Site Settings from it.
  var showingMenu = false
  /// The bottom bar shrunk to a small address pill while the page scrolls
  /// down (Site Settings → Collapse Bottom Bar).
  private(set) var barCollapsed = false
  @ObservationIgnored private var scrolled: CGFloat = 0
  var showingSiteSettings = false
  /// The command bar opens a new tab (the "+" buttons), not the shown tab's
  /// address (the address pill).
  var commandBarNewTab = true
  var showingSettings = false
  /// The downloads list, from the Lock Screen's download activity.
  var showingDownloads = false
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
    extensions = WebExtensions(directory: directory)
    tabs = TabManager(store: store, siteSettings: siteSettings, contentBlocking: contentBlocking)
    system = SystemBridge()
    let history = (try? HistoryStore(path: directory.appendingPathComponent("History.sqlite").path)) ?? (try! HistoryStore())
    history.expire()
    sync = SyncController(store: store, history: history, passwords: passwords, directory: directory)
    selectedSpaceID = store.spaces.first?.id
    tabs.downloads = downloads
    tabs.extensions = extensions
    extensions.app = self
    system.app = self
    downloads.activities = DownloadActivities()
    Favicons.shared.onStored = { [weak self] host in self?.system.iconStored(host: host) }
    IntentRouter.handle = { [weak self] link in self?.system.handle(link) }
    IntentRouter.cancelDownload = { [weak self] id in UUID(uuidString: id).map { self?.downloads.cancel($0) } }
    contentBlocking.onChange = { [weak self] in self?.tabs.applyContentBlockingToAll() }
    Task { [contentBlocking] in await contentBlocking.refresh() }
    tabs.openInNewTab = { [weak self] url in self?.open(url.absoluteString) }
    tabs.onPageLoaded = { [weak self] url, title, id in
      guard let self else { return }
      self.sync.recordVisit(url: url, title: title, profile: self.accountProfile(ofTab: id))
    }
    tabs.onPasswordMessage = { [weak self] body, id, url in
      guard let self else { return }
      self.passwords.handle(body, tabID: id, pageURL: url, profile: self.accountProfile(ofTab: id))
    }
    tabs.onNavigated = { [weak self] id in
      self?.passwords.pageChanged(id)
      self?.setBarCollapsed(false)
    }
    tabs.onScroll = { [weak self] id, dy, user, atTop in self?.pageScrolled(id, dy, user: user, atTop: atTop) }
    siteSettings.onChange = { [weak self] option, site in
      guard let self else { return }
      tabs.apply(option, site: site)
      if option == .collapseBar { setBarCollapsed(false) }
    }
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
      //   -CompassOpenLink <url>: as a link from another app.
      if let link = defaults.string(forKey: "CompassOpenLink").flatMap(URL.init(string:)) { openLink(link) }
      //   -CompassAddExtension <store link or ID>: add it, as "Add"
      //   would, without the sheet (docs/design/ios.md, "Web extensions").
      //   A path (starting with /) adds a package file.
      if let link = defaults.string(forKey: "CompassAddExtension") {
        Task { [weak self] in
          guard let self else { return }
          if link.hasPrefix("/") {
            await self.extensions.fetch(file: URL(fileURLWithPath: link))
          } else if let source = ExtensionSource(link) {
            await self.extensions.fetch(source)
          }
          // -CompassExtensionSheet YES: stop at the install sheet.
          if self.extensions.pending != nil, !defaults.bool(forKey: "CompassExtensionSheet") {
            await self.extensions.confirmPending()
            Presenter.dismissTop()
          }
          if let error = self.extensions.installError { print("CompassAddExtension: \(error)") }
        }
      }
      //   -CompassExtensionAction <n>: after 20 s (or
      //   -CompassExtensionActionDelay <s>), tap the menu's nth extension
      //   button (its popup comes up).
      if #available(iOS 18.4, *), let index = defaults.object(forKey: "CompassExtensionAction").flatMap({ Int("\($0)") }) {
        Task { [weak self] in
          let delay = defaults.object(forKey: "CompassExtensionActionDelay").flatMap { Double("\($0)") } ?? 20
          try? await Task.sleep(for: .seconds(delay))
          guard let self else { return }
          let actions = self.extensions.actions(forTab: self.shownTab?.id)
          if index < actions.count { self.extensions.performAction(actions[index].record.id, tab: self.shownTab?.id) }
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
      //   -CompassSettings YES: open Settings.
      if defaults.bool(forKey: "CompassSettings") {
        Task { [weak self] in
          try? await Task.sleep(for: .milliseconds(1500))
          self?.showingSettings = true
        }
      }
      //   -CompassMenu YES: open the bottom bar's menu.
      if defaults.bool(forKey: "CompassMenu") {
        Task { [weak self] in
          try? await Task.sleep(for: .milliseconds(1500))
          self?.showingMenu = true
        }
      }
      //   -CompassOpenURLs "<url> <url>…": open each as a tab, one after
      //   another (so each has been shown, and has a snapshot).
      //   -CompassPicker YES: then open the tab picker.
      let urls = defaults.string(forKey: "CompassOpenURLs")?.split(separator: " ").map(String.init) ?? []
      let picker = defaults.bool(forKey: "CompassPicker")
      if !urls.isEmpty || picker {
        Task { [weak self] in
          for url in urls {
            self?.open(url)
            try? await Task.sleep(for: .seconds(3))
          }
          try? await Task.sleep(for: .milliseconds(1500))
          if picker { self?.openPicker() }
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
    observeSessions()
    // What the extensions asked for while the app wasn't running.
    system.writeSnapshot()
    system.sharedChanged()
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
      _ = store.spaces.map { ($0.name, $0.theme, $0.allNodes.map { ($0.url, $0.title, $0.children.count) }) }
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
      self.system.scheduleSnapshot()
    }
  }

  /// Other devices' tabs changed: the widget that shows them follows.
  private func observeSessions() {
    withObservationTracking {
      _ = sync.sessionsByAccount
      _ = sync.isOn
    } onChange: { [weak self] in
      Task { @MainActor in
        self?.system.scheduleSnapshot()
        self?.observeSessions()
      }
    }
  }

  /// The spaces the sidebar pages through: all of them, or only the Focus
  /// filter's while its Focus is on with "Show only this space".
  var visibleSpaces: [Space] {
    if let focus = system.focus, focus.onlyThisSpace, let space = store.space(id: focus.spaceID) { return [space] }
    return store.spaces
  }

  /// Whatever is over the browser closes (before a link from outside).
  func dismissOverlays() {
    showingCommandBar = false
    showingMenu = false
    showingSettings = false
    showingSiteSettings = false
    showingDownloads = false
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
    // Its card in the tab picker shows the page as it was left.
    let previous = shownTab
    if let shown = previous, shown !== node { tabs.snapshot(shown.id) }
    if let split = node.parent, split.isSplit {
      selectedNodeID = split.id
      selectedPane = split.children.firstIndex { $0 === node } ?? 0
    } else {
      selectedNodeID = node.id
      selectedPane = 0
    }
    store.touch(node)
    if previous !== node, node.isTab { extensions.tabActivated(node.id, previous: previous?.id) }
    setBarCollapsed(false)
    showingCommandBar = false
    showingSidebar = false
    sidebarHalf = false
  }

  /// The shown page scrolled: down by the user collapses the bar, up (or
  /// back at the top) brings it back. A scroll has to go 24 pt one way.
  private func pageScrolled(_ id: String, _ dy: CGFloat, user: Bool, atTop: Bool) {
    guard id == shownTab?.id else { return }
    if atTop {
      setBarCollapsed(false)
      return
    }
    // Sub-point jitter during a drag goes both ways: it mustn't reset the
    // count.
    guard user, abs(dy) >= 1 else { return }
    let site = SiteSettings.site(tabs.live[id]?.url)
    guard siteSettings.value(.collapseBar, site: site) else { return }
    if (dy > 0) != (scrolled > 0) { scrolled = 0 }
    scrolled += dy
    if scrolled > 24 {
      setBarCollapsed(true)
    } else if scrolled < -24 {
      setBarCollapsed(false)
    }
  }

  func setBarCollapsed(_ collapsed: Bool) {
    guard barCollapsed != collapsed else { return }
    withAnimation(.snappy(duration: 0.25)) { barCollapsed = collapsed }
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
  /// A link from another app: whatever was up closes, and the page opens
  /// as it is, in a new Today tab of the shown space.
  func openLink(_ url: URL, in target: Space? = nil) {
    dismissOverlays()
    guard let space = target ?? selectedSpace ?? store.spaces.first,
      let tab = store.addTab(url: url.absoluteString, to: space.today)
    else { return }
    selectedSpaceID = space.id
    extensions.tabOpened(tab.id)
    show(tab)
  }

  func open(_ text: String, replacing node: SidebarNode? = nil) {
    let url = Self.url(for: text)
    if let node, node.isTab {
      store.update(node, url: url)
      if let view = tabs.live[node.id], let u = URL(string: url) { view.load(URLRequest(url: u)) }
      show(node)
      return
    }
    guard let space = selectedSpace, let tab = store.addTab(url: url, to: space.today) else { return }
    extensions.tabOpened(tab.id)
    show(tab)
  }

  /// `text` as typed in the command bar: an address, or else a search with
  /// the chosen engine.
  static func url(for text: String) -> String {
    address(text) ?? SearchEngine.current.url(for: text.trimmingCharacters(in: .whitespacesAndNewlines))
  }

  /// `text` as an address, if it is one ("example.com", a full URL).
  static func address(_ text: String) -> String? {
    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    if let url = URL(string: trimmed), url.scheme != nil, url.host() != nil { return trimmed }
    if !trimmed.contains(" "), trimmed.contains("."), let url = URL(string: "https://" + trimmed), url.host() != nil {
      return url.absoluteString
    }
    return nil
  }

  /// The picker's cards are set up (the shown tab centered) as a finger
  /// starts opening it.
  func preparePicker() {
    pickerHolding = false
    if let shown = shownTab { tabs.snapshot(shown.id) }
    pickerTabs = tabs.loadedOrder.filter { store.node(id: $0) != nil }
    let index = shownTab.flatMap { pickerTabs.firstIndex(of: $0.id) } ?? pickerTabs.count - 1
    pickerPosition = CGFloat(max(index, 0))
  }

  func openPicker() {
    preparePicker()
    withAnimation(.snappy(duration: 0.35)) { showingPicker = true }
  }

  /// A finger let go of the opening (or closing) picker at `position`,
  /// heading for `predicted` (both 0...1). Closed, it shows the centered card.
  func settlePicker(predicted: CGFloat) {
    if predicted > 0.5 {
      withAnimation(.snappy(duration: 0.35)) {
        pickerDrag = nil
        showingPicker = true
      }
    } else {
      pickTab(at: pickerTabs.isEmpty ? nil : Int(pickerPosition.rounded()))
    }
  }

  /// The picker closes onto card `index` (it grows back to the page), or
  /// onto what's shown when nil. The card grows first; the tab switches
  /// under it once it fills the screen (attaching a web view and updating
  /// the sidebar mid-animation made it stutter), and the picker goes a
  /// moment later, when the page has drawn.
  func pickTab(at index: Int?) {
    var target: SidebarNode?
    if let index, pickerTabs.indices.contains(index) {
      target = store.node(id: pickerTabs[index])
    }
    pickerHolding = true
    withAnimation(.snappy(duration: 0.35)) {
      if let index { pickerPosition = CGFloat(index) }
      pickerDrag = nil
      showingPicker = false
    } completion: { [weak self] in
      guard let self else { return }
      if let target, target !== self.shownTab, self.store.node(id: target.id) != nil { self.show(target) }
      Task { [weak self] in
        try? await Task.sleep(for: .milliseconds(120))
        guard let self, !self.showingPicker, self.pickerDrag == nil else { return }
        self.pickerHolding = false
      }
    }
  }

  /// A card flicked away: a Today entry goes to the archive, as on the
  /// desktop; anything else (pinned, a favorite, a split's pane) unloads.
  func closeFromPicker(_ id: String) {
    guard let index = pickerTabs.firstIndex(of: id) else { return }
    if let node = store.node(id: id) {
      if node.section == .today, node.parent?.isSplit != true { store.archive(node) }
      // Not reloaded behind the picker: the page goes empty.
      if shownTab === node { selectedNodeID = nil }
    }
    tabs.unload(id)
    pickerTabs.remove(at: index)
    if pickerTabs.isEmpty {
      // Nothing left to pick: back to the (empty) page.
      pickerPosition = 0
      pickTab(at: nil)
    } else if CGFloat(index) < pickerPosition || Int(pickerPosition) >= pickerTabs.count {
      pickerPosition = max(pickerPosition - 1, 0)
    }
  }

  func close(_ node: SidebarNode) {
    if selectedNodeID == node.id { selectedNodeID = nil }
    for tab in node.tabs {
      tabs.unload(tab.id)
      extensions.tabClosed(tab.id)
    }
    store.remove(node)
  }
}
