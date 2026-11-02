// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
import CompassShared
import Foundation
import SwiftUI
import Observation

/// One window's state: the space and entry it shows, its sidebar, sheets
/// and tab picker. What windows share (the store, live tabs, sync…) is
/// `AppServices`; the properties below forward to it, so views use one
/// model. An iPhone has one window; an iPad can have several.
@MainActor
@Observable
final class AppModel {
  let services: AppServices
  let id = UUID()
  var store: SpaceStore { services.store }
  var tabs: TabManager { services.tabs }
  var sync: SyncController { services.sync }
  var passwords: PasswordManager { services.passwords }
  var siteSettings: SiteSettings { services.siteSettings }
  var contentBlocking: ContentBlocking { services.contentBlocking }
  var downloads: Downloads { services.downloads }
  var extensions: WebExtensions { services.extensions }
  var directory: URL { services.directory }
  /// Widgets, the share sheet, Spotlight, the Focus filter (App/System).
  var system: SystemBridge { services.system }

  /// The space the sidebar shows (to extensions, the focused window).
  var selectedSpaceID: String? {
    didSet {
      if let id = selectedSpaceID, id != oldValue, services.active === self {
        extensions.spaceFocused(id)
        system.spaceShown(id)
        UserDefaults.standard.set(id, forKey: StartScreen.spaceKey)
      }
    }
  }
  /// The entry the browser shows; nil = an empty page.
  var selectedNodeID: String? { didSet { shownTabChanged() } }
  /// The sidebar, a bottom sheet over the browser. Up at launch.
  var showingSidebar = true
  /// A wide window (iPad, regular width): the sidebar is a column beside
  /// the page instead (RootView), shown or hidden by `showingSidebarColumn`,
  /// and splits show their panes side by side.
  var isWide = false
  var showingSidebarColumn = true
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
  var selectedPane = 0 { didSet { shownTabChanged() } }
  /// The tab shown last: leaving it starts auto picture-in-picture.
  @ObservationIgnored private var lastShownTabID: String?
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
  /// This window's Claude chat (docs/design/claude.md), and its sheet.
  let claude = ClaudeChat()
  var showingClaude = false

  /// Side-effect free: SwiftUI may make a window's model more than once.
  /// `attach` starts it.
  init(services: AppServices) {
    self.services = services
  }

  /// The window is up: it joins the app's windows and shows `entry` (a
  /// window opened for an entry, or restored), else the space last shown.
  /// The first window also runs the launch-time things: Settings → On
  /// Launch and the debug arguments.
  func attach(showing entry: String?) {
    let first = services.windows.isEmpty
    services.attach(self)
    selectedSpaceID = (services.active?.selectedSpaceID).flatMap(store.space(id:))?.id
      ?? (UserDefaults.standard.string(forKey: StartScreen.spaceKey).flatMap(store.space(id:)) ?? store.spaces.first)?.id
    if let entry, let node = store.node(id: entry), !node.isFolder {
      show(node)
      return
    }
    guard first else { return }
    // Settings → On Launch: the tab last shown, if it's still open (not
    // closed or archived), instead of the sidebar.
    if StartScreen.current == .lastTab, let id = UserDefaults.standard.string(forKey: StartScreen.tabKey),
      let node = store.node(id: id), node.isTab
    {
      show(node)
    }
    launchArguments()
  }

  func detach() { services.detach(self) }

  /// The tabs this window shows: the shown one, or every pane of a split
  /// side by side (a wide window).
  var displayedTabIDs: [String] {
    if isWide, let node = selectedNode, node.isSplit { return node.children.filter(\.isPhoneVisible).map(\.id) }
    return shownTab.map { [$0.id] } ?? []
  }

  /// Whether the tab's page is in this window, or another window has it
  /// (this one shows its snapshot and "Bring here").
  func hasPage(_ tabID: String) -> Bool { services.owner(of: tabID).map { $0 === self } ?? true }

  /// "Bring here": the page leaves the other window for this one.
  func bringHere(_ tabID: String) { services.claim(tabID, for: self) }

  var sidebarURL: URL { services.sidebarURL }
  func accountProfile(ofTab id: String) -> String { services.accountProfile(ofTab: id) }
  func scheduleSave() { services.scheduleSave() }

  private func launchArguments() {
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
      //   -CompassClaude YES: open the Claude sheet.
      if defaults.bool(forKey: "CompassClaude") {
        Task { [weak self] in
          try? await Task.sleep(for: .milliseconds(4000))
          self?.showingClaude = true
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
      //   -CompassSplit YES: then split the last two into one entry.
      let urls = defaults.string(forKey: "CompassOpenURLs")?.split(separator: " ").map(String.init) ?? []
      let picker = defaults.bool(forKey: "CompassPicker")
      let split = defaults.bool(forKey: "CompassSplit")
      if !urls.isEmpty || picker {
        Task { [weak self] in
          var opened: [SidebarNode] = []
          for url in urls {
            self?.open(url)
            if let tab = self?.shownTab { opened.append(tab) }
            try? await Task.sleep(for: .seconds(3))
          }
          try? await Task.sleep(for: .milliseconds(1500))
          if picker { self?.openPicker() }
          if split, opened.count >= 2, let entry = self?.store.split(opened[opened.count - 2], with: opened[opened.count - 1]),
            let first = entry.children.first
          {
            self?.show(first)
          }
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
  /// Auto picture-in-picture (AutoPiP): the tab left shows its playing
  /// video in a window, the tab shown takes its own back.
  private func shownTabChanged() {
    let shown = shownTab?.id
    guard shown != lastShownTabID else { return }
    let left = lastShownTabID
    lastShownTabID = shown
    // Picked here, or the window the user is in: the page comes here.
    for id in displayedTabIDs
    where services.active === self || !services.windows.contains(where: { $0 !== self && $0.displayedTabIDs.contains(id) }) {
      services.claim(id, for: self)
    }
    if let shown { UserDefaults.standard.set(shown, forKey: StartScreen.tabKey) }
    else { UserDefaults.standard.removeObject(forKey: StartScreen.tabKey) }
    if let shown { AutoPiP.exit(tabs.live[shown]) }
    // Kept in the window first (SwiftUI drops it on this update), or its
    // window would end at once.
    if let left, tabs.playingVideo.contains(left), AutoPiP.isOn, hasPage(left), let view = tabs.live[left] {
      AutoPiP.park(view)
      AutoPiP.enter(view)
    }
  }

  /// Scroll-out picture-in-picture, once the page stands still (finger up,
  /// momentum over): a window started mid-scroll grows from a frame the
  /// page has already moved past, and jumps. Then the page says what it
  /// wants now, which the scroll may have changed.
  @ObservationIgnored private var pictureInPictureSettle: Set<String> = []

  func settlePictureInPicture(_ id: String) {
    guard pictureInPictureSettle.insert(id).inserted else { return }
    Task { [weak self] in
      while let scroll = self?.tabs.live[id]?.scrollView,
        scroll.isTracking || scroll.isDragging || scroll.isDecelerating
      {
        try? await Task.sleep(for: .milliseconds(50))
      }
      guard let self else { return }
      self.pictureInPictureSettle.remove(id)
      guard self.shownTab?.id == id, let view = self.tabs.live[id] else { return }
      let want = try? await view.evaluateJavaScript(
        "window.__compassAutoPiPWant ? window.__compassAutoPiPWant() : ''", in: nil, contentWorld: AutoPiP.world)
      guard self.shownTab?.id == id else { return }
      switch want as? String {
      case "in": AutoPiP.exit(view)
      case "out":
        guard AutoPiP.scrollIsOn, UIApplication.shared.applicationState == .active, view.window != nil else { return }
        AutoPiP.enter(view, scroll: true)
      default: break
      }
    }
  }

  /// Back in the app: a video Compass moved to picture-in-picture comes
  /// back. Leaving the app doesn't start one: iOS ends a window an app on
  /// its way out starts (checked on an iPhone, iOS 27); the page's
  /// `autoPictureInPicture` is set in case WebKit honors it.
  func appActiveChanged(_ active: Bool) {
    guard active, let id = shownTab?.id else { return }
    AutoPiP.exit(tabs.live[id])
  }

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
  func pageScrolled(_ id: String, _ dy: CGFloat, user: Bool, atTop: Bool) {
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
    if isWide {
      showingSidebarColumn = true
      return
    }
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

  /// A space's entries in the bar's swipe order: pinned, then Today
  /// (newest first); splits as one entry, desktop-only pages left out.
  func swipeEntries(of space: Space) -> [SidebarNode] {
    (space.pinned.children.flatMap(\.subtree) + space.today.children.reversed())
      .filter { !$0.isFolder && $0.parent?.isSplit != true && $0.isPhoneVisible }
  }

  /// The entry `delta` steps from `node` in its space (the bar's swipe, ⌃Tab).
  func neighbor(of node: SidebarNode, _ delta: Int) -> SidebarNode? {
    guard let space = store.space(of: node) else { return nil }
    let entries = swipeEntries(of: space)
    guard let index = entries.firstIndex(where: { $0 === node }) else { return nil }
    let next = index + delta
    return entries.indices.contains(next) ? entries[next] : nil
  }

  /// ⌃Tab / ⌃⇧Tab: the next or previous entry, wrapping around; the first
  /// or last when nothing is shown.
  func stepEntry(_ delta: Int) {
    guard let space = selectedSpace else { return }
    let entries = swipeEntries(of: space)
    guard !entries.isEmpty else { return }
    let index = selectedNode.flatMap { node in entries.firstIndex(where: { $0 === node }) }
    let next = index.map { ($0 + delta + entries.count) % entries.count } ?? (delta > 0 ? 0 : entries.count - 1)
    show(entries[next])
  }

  /// ⌘W: the shown entry closes as a flicked tab card does: Today goes to
  /// the archive, anything else (pinned, a favorite) unloads.
  func closeShown() {
    guard let node = selectedNode else { return }
    if node.section == .today { store.archive(node) }
    selectedNodeID = nil
    for tab in node.tabs { tabs.unload(tab.id) }
  }

  /// ⌃1–9: the nth space of the sidebar.
  func selectSpace(at index: Int) {
    let spaces = visibleSpaces
    guard spaces.indices.contains(index) else { return }
    selectedSpaceID = spaces[index].id
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

/// What a launch shows (Settings → On Launch): the sidebar over an empty
/// page, or the tab last shown with the sidebar down.
enum StartScreen: String, CaseIterable, Identifiable {
  case sidebar, lastTab

  static let key = "StartScreen"
  /// The tab and the space last shown, kept as they change.
  static let tabKey = "LastShownTab"
  static let spaceKey = "LastShownSpace"

  static var current: StartScreen { UserDefaults.standard.string(forKey: key).flatMap(Self.init) ?? .sidebar }

  var id: String { rawValue }
  var name: String {
    switch self {
    case .sidebar: "Sidebar"
    case .lastTab: "Last Tab"
    }
  }
}

