// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import CompassModel
import CompassShared
import Foundation
import LocalAuthentication
import Observation
import SwiftUI
import UIKit

/// What every window shares: the sidebar's store, the live tabs, sync,
/// passwords, extensions, downloads and the system bridges. One per app;
/// each window has its own `AppModel` (what it shows, its sheets) on top.
///
/// Windows on iPad (docs/design/ios.md, "Multiple windows"): a tab's web
/// view can be in one window at a time. It belongs to the window that last
/// took it (`claim`): the active (key) window when it shows the tab, or a
/// window where it was picked. Another window showing the same entry shows
/// its snapshot with "Bring here", as the desktop's placeholder does.
@MainActor
@Observable
final class AppServices {
  static private(set) var shared: AppServices!

  let store = SpaceStore()
  let tabs: TabManager
  let sync: SyncController
  let passwords = PasswordManager()
  /// What the user closed, for undo and redo (every window's).
  let closed = ClosedEntries()
  let siteSettings: SiteSettings
  let contentBlocking = ContentBlocking()
  let downloads = Downloads()
  /// Reader mode: which pages read as articles, and the reader's look.
  let reader = ReaderMode()
  let extensions: WebExtensions
  /// Live folders' plugins, refreshes and secrets.
  let liveFolders: LiveFolders
  let directory: URL
  /// Widgets, the share sheet, Spotlight, the Focus filter (App/System).
  let system: SystemBridge
  /// Claude's small background requests for the sidebar, and the link
  /// folders' names (docs/design/ai-sidebar.md).
  let quick: ClaudeQuick
  let namer: LinkFolderNamer
  /// Tidy Today (docs/design/ai-sidebar.md), and its undos.
  let tidier: TodayTidier
  /// Shorter tab titles in the sidebar's rows (docs/design/ai-sidebar.md).
  let shortener: TabTitleShortener

  /// The open windows, in the order they opened.
  private(set) var windows: [AppModel] = []
  /// The key window (the one the user is in); nil before the first one.
  private(set) weak var active: AppModel?
  /// Which window each tab's web view belongs to (`AppModel.id`).
  private var holder: [String: UUID] = [:]
  /// Little windows' pages (iPad) waiting for their scene to come up,
  /// oldest first (`LittleWindowScene` takes them).
  @ObservationIgnored private var pendingLittle: [Peek] = []
  /// A little window's page that's opening in a space while no browser
  /// window is open: the next one to come up takes it.
  @ObservationIgnored private var pendingAdopt: (peek: Peek, space: Space?)?
  /// Entries dragged out of a window (TabDrag), by the session of the
  /// window iPadOS opened for them, until that window takes its entry.
  @ObservationIgnored private var draggedOut: [String: String] = [:]
  /// The little window the user is in: the keyboard commands act on its
  /// page. Nil once a browser window is the key one.
  private(set) weak var keyLittle: Peek?

  /// Incognito is behind Face ID: since the app last went to the
  /// background with incognito tabs open.
  private(set) var incognitoLocked = false
  /// Incognito is covered (the app is inactive: the app switcher's
  /// snapshot mustn't show it).
  private(set) var incognitoCovered = false

  @ObservationIgnored private var saveTask: Task<Void, Never>?
  @ObservationIgnored private var archiveTimer: Task<Void, Never>?

  static func start() -> AppServices {
    if let shared { return shared }
    let services = AppServices()
    shared = services
    return services
  }

  private init() {
    directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Compass", isDirectory: true)
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let store = self.store
    // Debug: -CompassFreshStore YES starts from an empty sidebar (UI tests).
    #if DEBUG
      let fresh = UserDefaults.standard.bool(forKey: "CompassFreshStore")
    #else
      let fresh = false
    #endif
    if !fresh { try? store.load(from: directory.appendingPathComponent("Sidebar.json")) }
    if store.spaces.isEmpty {
      store.applyingRemote { _ = store.addSpace(name: "Home") }
    }
    siteSettings = SiteSettings(directory: directory)
    extensions = WebExtensions(directory: directory)
    tabs = TabManager(store: store, siteSettings: siteSettings, contentBlocking: contentBlocking)
    liveFolders = LiveFolders(store: store, tabs: tabs, directory: directory)
    #if DEBUG
      // -CompassLiveFolderDemo <feed URL>: an RSS live folder in the first
      // space's Pinned, open (trying the runtime in the simulator).
      if let feed = UserDefaults.standard.string(forKey: "CompassLiveFolderDemo"),
        let rss = liveFolders.plugin(id: "compass.rss"), let space = store.spaces.first,
        !space.pinned.subtree.contains(where: \.isLiveFolder)
      {
        liveFolders.create(plugin: rss, name: "", config: ["url": feed], secrets: [:], grantCookies: false, in: space.pinned)
      }
    #endif
    system = SystemBridge()
    quick = ClaudeQuick(
      endpoint: { ClaudeSettings.shared.endpoint },
      isOn: { ClaudeSettings.shared.isOn($0) },
      cacheLevel: { CacheLevelMemory().level(for: $0.baseURL, isClaudeAPI: $0.isClaudeAPI) })
    namer = LinkFolderNamer(
      store: store, quick: quick, prompts: ClaudeSettings.sidebarPrompts,
      model: { ClaudeSettings.shared.nameLinkFoldersModel })
    tidier = TodayTidier(
      store: store, quick: quick, prompts: ClaudeSettings.sidebarPrompts,
      model: { ClaudeSettings.shared.tidyTodayModel }, isShown: { $0.isPhoneVisible })
    shortener = TabTitleShortener(
      store: store, quick: quick, prompts: ClaudeSettings.sidebarPrompts,
      model: { ClaudeSettings.shared.shortTabTitlesModel })
    let history = (try? HistoryStore(path: directory.appendingPathComponent("History.sqlite").path)) ?? (try! HistoryStore())
    history.expire()
    sync = SyncController(store: store, history: history, passwords: passwords, directory: directory)
    tabs.downloads = downloads
    tabs.extensions = extensions
    downloads.activities = DownloadActivities()
    Favicons.shared.onStored = { [weak self] host in self?.system.iconStored(host: host) }
    Favicons.shared.isIncognitoHost = { [weak self] host in
      self?.store.incognito.allNodes.contains { Favicons.host(of: $0.url) == host } ?? false
    }
    IntentRouter.handle = { [weak self] link in self?.system.handle(link) }
    IntentRouter.cancelDownload = { [weak self] id in UUID(uuidString: id).map { self?.downloads.cancel($0) } }
    contentBlocking.onChange = { [weak self] in self?.tabs.applyContentBlockingToAll() }
    Task { [contentBlocking] in await contentBlocking.refresh() }
    // A page's window.open: a new tab in the window the user is in.
    tabs.openInNewTab = { [weak self] url in self?.active?.open(url.absoluteString) }
    // Peek: the card shows in the window of the tab the page came from.
    tabs.onPeek = { [weak self] page, source in
      guard let self, let window = self.owner(of: source) ?? self.active else { return }
      window.showPeek(page, from: source)
    }
    // A link's long-press items: in the window of the tab it's in.
    tabs.onLinkAction = { [weak self] action, url, source in
      guard let self, let window = self.owner(of: source) ?? self.active else { return }
      window.open(action, url, from: source)
    }
    tabs.onPageClosed = { [weak self] page in
      for window in self?.windows ?? [] where window.peek?.page === page { window.closePeek() }
    }
    tabs.onPageLoaded = { [weak self] url, title, id in
      // Reader mode, incognito too (it stays on the device).
      if let self, let view = self.tabs.live[id] { self.reader.check(id, view) }
      guard let self, !self.tabs.isIncognito(id) else { return }  // no history in incognito
      self.sync.recordVisit(url: url, title: title, profile: self.accountProfile(ofTab: id))
    }
    tabs.onPasswordMessage = { [weak self] body, id, url in
      guard let self else { return }
      self.passwords.handle(
        body, tabID: id, pageURL: url, profile: self.accountProfile(ofTab: id), incognito: self.tabs.isIncognito(id))
    }
    tabs.onPictureInPictureRestore = { [weak self] id in
      guard let self, let node = self.store.node(id: id) else { return }
      let window = self.owner(of: id) ?? self.active
      guard let window, window.shownTab?.id != id else { return }
      window.show(node)
    }
    // Only the tab shown, with the app active: picture-in-picture can't
    // start on the way out.
    tabs.onPictureInPictureScroll = { [weak self] id, _ in self?.owner(of: id)?.settlePictureInPicture(id) }
    tabs.onNavigated = { [weak self] id in
      self?.passwords.pageChanged(id)
      self?.owner(of: id)?.setBarCollapsed(false)
      // Another page: it's checked again (Reader mode).
      if let self, let view = self.tabs.live[id] { self.reader.pageChanged(id, view) }
    }
    // A new document leaves Reader, as on the desktop.
    tabs.onCommitted = { [weak self] id in
      for window in self?.windows ?? [] { window.closeReader(id) }
    }
    tabs.onScroll = { [weak self] id, dy, user, atTop in
      self?.owner(of: id)?.pageScrolled(id, dy, user: user, atTop: atTop)
    }
    siteSettings.onChange = { [weak self] option, site in
      guard let self else { return }
      tabs.apply(option, site: site)
      if option == .collapseBar { for window in windows { window.setBarCollapsed(false) } }
    }
    Favicons.shared.onPageIcon = { [weak self] host, png, size, tab in
      guard let self, !self.tabs.isIncognito(tab) else { return }
      self.sync.offerFavicon(host: host, png: png, sourceSize: size, account: self.accountProfile(ofTab: tab))
    }
    sync.isShown = { [weak self] id in self?.windows.contains { $0.selectedNodeID == id } ?? false }
    sync.onReplaced = { [weak self] old, new in
      guard let self else { return }
      self.tabs.rekey(old, to: new)
      if let window = self.holder.removeValue(forKey: old) { self.holder[new] = window }
      for window in self.windows where window.selectedNodeID == old { window.selectedNodeID = new }
    }
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

  /// Ask in the command bar (docs/design/ai-sidebar.md): one per command
  /// bar, streaming through the chat's client (ClaudeClient).
  func makeAsker() -> CommandBarAsker {
    CommandBarAsker(
      quick: quick, prompts: ClaudeSettings.sidebarPrompts, model: { ClaudeSettings.shared.commandBarAskModel },
      stream: { body, endpoint, onEvent in
        await ClaudeClient.stream(ChatRequest(body: body), endpoint: endpoint, onEvent: onEvent)
      })
  }

  var sidebarURL: URL { directory.appendingPathComponent("Sidebar.json") }

  /// The profile account a tab's history and passwords go to (its space's).
  func accountProfile(ofTab id: String) -> String {
    store.node(id: id).map(sync.accountProfile(of:)) ?? ""
  }

  // MARK: Windows

  func attach(_ window: AppModel) {
    guard !windows.contains(where: { $0 === window }) else { return }
    windows.append(window)
    if active == nil { activate(window) }
  }

  /// A window closed: its tabs stay live (and loaded) for the others.
  func detach(_ window: AppModel) {
    windows.removeAll { $0 === window }
    holder = holder.filter { $0.value != window.id }
    if active === window { active = nil; if let next = windows.last { activate(next) } }
  }

  /// `window` is the key window now: what the app-wide things (extensions'
  /// focused window, widgets' links, shortcuts, sent tabs) act on, and it
  /// takes back the tabs it shows (move on focus).
  func activate(_ window: AppModel) {
    keyLittle = nil
    if active !== window {
      active = window
      extensions.app = window
      system.app = window
      AppDelegate.app = window
      SentTabNotifications.shared.app = window
      if let space = window.selectedSpaceID {
        extensions.spaceFocused(space)
        system.spaceShown(space)
      }
    }
    for id in window.displayedTabIDs { claim(id, for: window) }
  }

  // MARK: Links from other apps

  /// A web link from another app (Compass the default browser, or the
  /// share sheet's little window) that arrived in `window`. A link routing
  /// rule (docs/design/link-routing.md) makes it a new Today tab of the
  /// rule's space, shown in a window already showing that space (iPad: the
  /// one it arrived in, else the active one, else any), else in `window`,
  /// which switches to that space. Without a rule, a little window
  /// (`littleWindows`), or a new Today tab of the shown space.
  func openLinkFromOtherApp(_ url: URL, in window: AppModel, littleWindows: Bool = LittleWindows.isOn) {
    // Between two profiles' rules for one domain, the shown space's.
    let profile = window.selectedSpace.flatMap { $0.isIncognito ? nil : $0.profileUUID }
    switch store.opening(forLinkFromAnotherApp: url, littleWindows: littleWindows, preferring: profile) {
    case .space(let id):
      guard let space = store.space(id: id) else { return window.openLink(url) }
      let showing = [window] + [active].compactMap { $0 } + windows
      let target = showing.first { $0.selectedSpaceID == id } ?? window
      target.openLink(url, in: space)
      guard target !== window else { return }
      activate(target)
      if let session = target.windowScene?.session {
        UIApplication.shared.activateSceneSession(for: UISceneSessionActivationRequest(session: session)) { _ in }
      }
    case .littleWindow:
      window.showLittleWindow(url)
    case .shownSpace:
      window.openLink(url)
    }
  }

  // MARK: Little windows (iPad)

  /// Asks iPadOS for a little window's scene, prominent (centered over the
  /// others), for `little`. If iPadOS refuses, `failed` (the card instead).
  func openLittleWindow(_ little: Peek, from scene: UIWindowScene?, failed: @escaping @MainActor () -> Void) {
    pendingLittle.append(little)
    let activity = NSUserActivity(activityType: LittleWindows.activityType)
    // What the little window's scene matches (handlesExternalEvents).
    activity.targetContentIdentifier = LittleWindows.activityType
    let options = UIWindowScene.ActivationRequestOptions()
    options.placement = UIWindowSceneProminentPlacement()
    options.requestingScene = scene
    var request = UISceneSessionActivationRequest(role: .windowApplication)
    request.userActivity = activity
    request.options = options
    UIApplication.shared.activateSceneSession(for: request) { [weak self] _ in
      Task { @MainActor in
        guard let self, let index = self.pendingLittle.firstIndex(where: { $0 === little }) else { return }
        self.pendingLittle.remove(at: index)
        failed()
      }
    }
  }

  /// A little window's scene came up: its page (nil for one iPadOS
  /// restored, which then closes).
  func takeLittle() -> Peek? { pendingLittle.isEmpty ? nil : pendingLittle.removeFirst() }

  func littleBecameKey(_ little: Peek) { keyLittle = little }

  // MARK: Entries dragged out (iPad)

  /// A scene is connecting for an entry dragged out of a window.
  func noteDraggedOut(_ entry: String, session: String) { draggedOut[session] = entry }

  /// The entry the window of `session` was opened for, once.
  func takeDraggedOut(for session: String) -> String? { draggedOut.removeValue(forKey: session) }

  /// Open in <space> from a little window: into the last active browser
  /// window, or the next one to open.
  func adoptLittle(_ little: Peek, in space: Space?) -> Bool {
    if keyLittle === little { keyLittle = nil }
    guard let window = active ?? windows.last else {
      pendingAdopt = (little, space)
      return false
    }
    window.adopt(little, in: space)
    // That window comes to the front, with the page.
    if let session = window.windowScene?.session {
      UIApplication.shared.activateSceneSession(for: UISceneSessionActivationRequest(session: session)) { _ in }
    }
    return true
  }

  /// A page waiting for a browser window (`adoptLittle`).
  func takePendingAdopt() -> (peek: Peek, space: Space?)? {
    defer { pendingAdopt = nil }
    return pendingAdopt
  }

  /// `window` takes the tab's web view. The window it leaves keeps a
  /// snapshot of the page as it was.
  func claim(_ tabID: String, for window: AppModel) {
    guard holder[tabID] != window.id else { return }
    if let from = owner(of: tabID), from !== window { tabs.snapshot(tabID) }
    holder[tabID] = window.id
  }

  /// The window the tab's web view is in: the one that took it last while
  /// it still shows it, or else the active window showing it, or else the
  /// first window showing it.
  func owner(of tabID: String) -> AppModel? {
    let showing = windows.filter { $0.displayedTabIDs.contains(tabID) }
    if let id = holder[tabID], let window = showing.first(where: { $0.id == id }) { return window }
    if let active, showing.contains(where: { $0 === active }) { return active }
    return showing.first
  }

  // MARK: Incognito

  var hasIncognitoTabs: Bool { !store.incognito.today.children.isEmpty || !store.incognito.pinned.children.isEmpty }

  /// The app's state changed: inactive covers incognito, the background
  /// locks it (if it has tabs), active uncovers it.
  func incognitoPhase(_ phase: ScenePhase) {
    switch phase {
    case .inactive: incognitoCovered = true
    case .background:
      incognitoCovered = true
      // Only where Face ID or a passcode can open it again.
      if hasIncognitoTabs, LAContext().canEvaluatePolicy(.deviceOwnerAuthentication, error: nil) { incognitoLocked = true }
    default: incognitoCovered = false
    }
  }

  /// Face ID (or the passcode) unlocks incognito.
  func unlockIncognito() async {
    let context = LAContext()
    context.localizedFallbackTitle = "Use Passcode"
    let ok = (try? await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: "Unlock your Incognito tabs")) ?? false
    if ok { incognitoLocked = false }
  }

  /// "Close All Incognito Tabs": every incognito entry goes, with its
  /// website data.
  func closeAllIncognito() {
    let incognito = store.incognito
    for window in windows where window.selectedSpace?.isIncognito == true || window.selectedNode.map(store.isIncognito) == true {
      window.selectedNodeID = nil
    }
    tabs.resetIncognito()
    for node in incognito.pinned.children + incognito.today.children { store.remove(node) }
    closed.dropIncognito()
    incognitoLocked = false
  }

  // MARK: Saving

  private func observeForSaving() {
    withObservationTracking {
      _ = store.spaces.map {
        ($0.name, $0.theme, $0.allNodes.map { ($0.url, $0.title, $0.children.count, $0.unseen, $0.liveSource, $0.nameSource) })
      }
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
}

