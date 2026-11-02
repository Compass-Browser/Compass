// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Observation
import WebKit

/// Live tabs: which sidebar entries have a WKWebView. At most `maxLive`
/// stay loaded; the least recently shown are unloaded (their entries stay,
/// and reload when shown). Each profile gets its own website data store, so
/// spaces of different profiles keep separate cookies, as on the desktop.
@MainActor
@Observable
final class TabManager: NSObject {
  static let maxLive = 6

  private(set) var live: [String: WKWebView] = [:]
  // Bookkeeping, not shown: unobserved, since views call webView(for:)
  // (which bumps `recent`) while rendering, and an observed write there
  // re-renders them forever.
  @ObservationIgnored private var recent: [String] = []
  @ObservationIgnored private var observations: [String: [NSKeyValueObservation]] = [:]
  /// Bumped when a live tab's state (loading, back/forward) changes.
  private(set) var revision = 0
  /// Tabs whose main-frame load hasn't had an answer yet (connecting: from
  /// the provisional navigation's start until it commits or fails).
  private(set) var connecting: Set<String> = []
  /// The last look of each live tab, for the tab picker's cards: taken as a
  /// tab is left and as the picker opens. In memory only; an unloaded tab
  /// loses it (the picker shows only live tabs).
  private(set) var snapshots: [String: UIImage] = [:]
  private let store: SpaceStore
  private let siteSettings: SiteSettings
  private let contentBlocking: ContentBlocking
  /// Where downloads go (set by AppModel).
  var downloads: Downloads?
  /// Web extensions: every tab's web view is attached to them (set by
  /// AppModel).
  @ObservationIgnored var extensions: WebExtensions?
  /// A link that wants a new window opens as a new Today entry.
  var openInNewTab: ((URL) -> Void)?
  /// A message from the password script: (body, tab ID, page URL).
  var onPasswordMessage: ((Any, String, URL?) -> Void)?
  /// A picture-in-picture window's "back to the page" (AutoPiP): tab ID.
  var onPictureInPictureRestore: ((String) -> Void)?
  /// A playing video scrolled off screen (true), or the one Compass moved
  /// for that back on screen (false): tab ID.
  var onPictureInPictureScroll: ((String, Bool) -> Void)?
  /// Tabs whose page plays a video with sound (AutoPiP's candidates).
  var playingVideo: Set<String> = []
  /// A tab's URL changed (its page's forms are gone).
  var onNavigated: ((String) -> Void)?
  /// The page scrolled: (tab ID, how far down since the last call, whether
  /// the user is scrolling it, whether it's at its top).
  var onScroll: ((String, CGFloat, Bool, Bool) -> Void)?
  /// A page finished loading (a history visit): (URL, title, tab ID).
  var onPageLoaded: ((String, String, String) -> Void)?

  init(store: SpaceStore, siteSettings: SiteSettings, contentBlocking: ContentBlocking) {
    self.store = store
    self.siteSettings = siteSettings
    self.contentBlocking = contentBlocking
  }

  /// The filter lists `site`'s settings allow, on `view` from its next load.
  func applyContentBlocking(_ view: WKWebView, site: String?) {
    let controller = view.configuration.userContentController
    controller.removeAllContentRuleLists()
    let lists = contentBlocking.lists(
      blockAds: siteSettings.value(.blockAds, site: site),
      blockCookieBanners: siteSettings.value(.blockCookieBanners, site: site))
    for list in lists { controller.add(list) }
  }

  /// New or updated filter lists: every open tab, for its site.
  func applyContentBlockingToAll() {
    for view in live.values { applyContentBlocking(view, site: SiteSettings.site(view.url)) }
  }

  func isLive(_ node: SidebarNode) -> Bool { live[node.id] != nil }

  /// The live tabs, least recently shown first.
  var loadedOrder: [String] { recent.filter { live[$0] != nil } }

  /// Takes the tab's snapshot (if it's live and on screen), at a card's
  /// resolution.
  func snapshot(_ id: String) {
    guard let view = live[id], view.window != nil, view.bounds.width > 0 else { return }
    let configuration = WKSnapshotConfiguration()
    configuration.afterScreenUpdates = false
    configuration.snapshotWidth = NSNumber(value: Double(min(view.bounds.width, 420)))
    view.takeSnapshot(with: configuration) { [weak self] image, _ in
      guard let self, let image, self.live[id] != nil else { return }
      self.snapshots[id] = image
    }
  }

  /// The web view of a tab entry, created and loaded on first use.
  func webView(for node: SidebarNode) -> WKWebView {
    if let view = live[node.id] {
      bump(node.id)
      return view
    }
    let configuration = WKWebViewConfiguration()
    // The home profile's spaces (and favorites) use the default store.
    let profile = store.profile(of: node)
    configuration.websiteDataStore = dataStore(for: store.isHome(profile) ? "" : profile)
    configuration.allowsInlineMediaPlayback = true
    configuration.allowsPictureInPictureMediaPlayback = true
    configuration.userContentController.addUserScript(PasswordScript.userScript)
    configuration.userContentController.addUserScript(DesktopViewport.script)
    configuration.userContentController.add(
      MessageProxy(self), contentWorld: PasswordScript.world, name: PasswordScript.handler)
    configuration.userContentController.addUserScript(AutoPiP.script)
    configuration.userContentController.add(MessageProxy(self), contentWorld: AutoPiP.world, name: AutoPiP.handler)
    let view = WKWebView(
      frame: .zero,
      configuration: extensions?.configure(configuration, for: URL(string: node.url)) ?? configuration)
    view.allowsBackForwardNavigationGestures = true
    view.isFindInteractionEnabled = true
    applyContentBlocking(view, site: SiteSettings.site(URL(string: node.url)))
    view.isInspectable = true
    view.navigationDelegate = self
    view.uiDelegate = self
    let id = node.id
    observe(view, as: id)
    live[id] = view
    bump(id)
    // Before it loads, so extensions know the tab its pages talk from (a
    // tab restored from the sidebar was never announced: 1Password's
    // background then found no tab for its messages).
    extensions?.tabOpened(id)
    if let url = URL(string: node.url), !node.url.isEmpty { view.load(URLRequest(url: url)) }
    return view
  }

  private func observe(_ view: WKWebView, as id: String) {
    observations[id] = [
      view.observe(\.url, options: [.new]) { [weak self] view, _ in
        Task { @MainActor in
          self?.changed(id, view)
          self?.extensions?.tabChanged(id, url: true)
        }
      },
      view.observe(\.title, options: [.new]) { [weak self] view, _ in
        Task { @MainActor in
          self?.changed(id, view)
          self?.extensions?.tabChanged(id, title: true)
        }
      },
      view.observe(\.estimatedProgress, options: [.new]) { [weak self] _, _ in
        Task { @MainActor in self?.revision += 1 }
      },
      view.observe(\.isLoading, options: [.new]) { [weak self] view, _ in
        Task { @MainActor in
          if !view.isLoading { self?.connecting.remove(id) }
          self?.revision += 1
          self?.extensions?.tabChanged(id, loading: true)
        }
      },
      // On the main thread, every frame of a scroll: no Task per call.
      view.scrollView.observe(\.contentOffset, options: [.old, .new]) { [weak self] scroll, change in
        guard let old = change.oldValue, let new = change.newValue else { return }
        MainActor.assumeIsolated {
          let inset = scroll.adjustedContentInset
          let atTop = new.y <= -inset.top + 4
          // Bouncing past either end isn't scrolling the page.
          let bottom = scroll.contentSize.height - scroll.bounds.height + inset.bottom
          let inBounds = new.y >= -inset.top && new.y <= max(bottom, -inset.top)
          let user = (scroll.isTracking || scroll.isDecelerating) && inBounds
          self?.onScroll?(id, new.y - old.y, user, atTop)
        }
      },
    ]
  }

  /// An entry got a new ID (kept after a remote close): its page follows.
  func rekey(_ old: String, to new: String) {
    guard let view = live.removeValue(forKey: old) else { return }
    observations[old] = nil
    snapshots[new] = snapshots.removeValue(forKey: old)
    observe(view, as: new)
    live[new] = view
    recent = recent.map { $0 == old ? new : $0 }
  }

  func unload(_ id: String) {
    observations[id] = nil
    connecting.remove(id)
    live[id]?.stopLoading()
    live[id] = nil
    snapshots[id] = nil
    recent.removeAll { $0 == id }
  }

  /// Drops web views whose entries are gone (closed here or elsewhere).
  func prune() {
    for id in live.keys where store.node(id: id) == nil { unload(id) }
  }

  /// A site setting changed (`site` nil: the default for every site): open
  /// tabs on the sites it reaches follow. Swipe to navigate at once; the
  /// desktop site with a reload (the content mode is chosen per load).
  func apply(_ option: SiteSettings.Option, site: String?) {
    for view in live.values {
      let viewSite = SiteSettings.site(view.url)
      guard site == nil || viewSite == site else { continue }
      switch option {
      case .swipeToNavigate:
        view.allowsBackForwardNavigationGestures = siteSettings.value(.swipeToNavigate, site: viewSite)
      case .desktopSite:
        view.reload()
      case .blockAds, .blockCookieBanners:
        applyContentBlocking(view, site: viewSite)
        view.reload()
      default:
        break
      }
    }
  }

  private func bump(_ id: String) {
    recent.removeAll { $0 == id }
    recent.append(id)
    while recent.count > Self.maxLive { unload(recent[0]) }
  }

  private func changed(_ id: String, _ view: WKWebView) {
    revision += 1
    onNavigated?(id)
    guard let node = store.node(id: id) else { return }
    store.update(node, url: view.url?.absoluteString, title: view.title)
  }

  private var stores: [String: WKWebsiteDataStore] = [:]

  /// Every profile's website data store: the home profile's (the default)
  /// and each other profile's in the sidebar.
  func allDataStores() -> [WKWebsiteDataStore] {
    let profiles = Set(store.spaces.map(\.profileUUID).filter { !store.isHome($0) })
    return [.default()] + profiles.sorted().map(dataStore(for:))
  }

  private func dataStore(for profile: String) -> WKWebsiteDataStore {
    guard !profile.isEmpty, let uuid = UUID(uuidString: profile) else { return .default() }
    if let store = stores[profile] { return store }
    let store = WKWebsiteDataStore(forIdentifier: uuid)
    stores[profile] = store
    return store
  }
}

extension TabManager: WKNavigationDelegate, WKUIDelegate {
  /// Each main-frame load gets its site's settings: the desktop site (the
  /// desktop user agent and a desktop-width viewport, so media queries lay
  /// the page out as on a computer).
  nonisolated func webView(
    _ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction,
    preferences: WKWebpagePreferences
  ) async -> (WKNavigationActionPolicy, WKWebpagePreferences) {
    await MainActor.run {
      // An extension's page in a tab going to a website (1Password's "Sign
      // in"): its web view is the extension's and won't load web pages, so
      // the tab starts over as a web page there.
      if webView.url?.scheme == "webkit-extension", navigationAction.targetFrame?.isMainFrame != false,
        let url = navigationAction.request.url, url.scheme == "https" || url.scheme == "http",
        let id = live.first(where: { $0.value === webView })?.key, let node = store.node(id: id)
      {
        store.update(node, url: url.absoluteString)
        Task { @MainActor in
          self.unload(id)
          self.revision += 1
        }
        return (.cancel, preferences)
      }
      // <a download> links.
      let policy: WKNavigationActionPolicy = navigationAction.shouldPerformDownload ? .download : .allow
      if navigationAction.targetFrame?.isMainFrame != false {
        let site = SiteSettings.site(navigationAction.request.url)
        preferences.preferredContentMode = siteSettings.value(.desktopSite, site: site) ? .desktop : .mobile
        webView.allowsBackForwardNavigationGestures = siteSettings.value(.swipeToNavigate, site: site)
        // Before the page's own requests go out.
        applyContentBlocking(webView, site: site)
      }
      return (policy, preferences)
    }
  }

  /// What the browser can't show, or the server sends as an attachment, is
  /// downloaded instead.
  nonisolated func webView(
    _ webView: WKWebView, decidePolicyFor navigationResponse: WKNavigationResponse
  ) async -> WKNavigationResponsePolicy {
    await MainActor.run {
      let disposition = (navigationResponse.response as? HTTPURLResponse)?
        .value(forHTTPHeaderField: "Content-Disposition")?.lowercased() ?? ""
      let download = !navigationResponse.canShowMIMEType || disposition.hasPrefix("attachment")
      return navigationResponse.isForMainFrame && download ? .download : .allow
    }
  }

  nonisolated func webView(
    _ webView: WKWebView, navigationAction: WKNavigationAction, didBecome download: WKDownload
  ) {
    MainActor.assumeIsolated { downloads?.track(download) }
  }

  nonisolated func webView(
    _ webView: WKWebView, navigationResponse: WKNavigationResponse, didBecome download: WKDownload
  ) {
    MainActor.assumeIsolated { downloads?.track(download) }
  }

  nonisolated func webView(_ webView: WKWebView, didStartProvisionalNavigation navigation: WKNavigation!) {
    MainActor.assumeIsolated { setConnecting(webView, true) }
  }

  nonisolated func webView(_ webView: WKWebView, didCommit navigation: WKNavigation!) {
    MainActor.assumeIsolated { setConnecting(webView, false) }
  }

  nonisolated func webView(
    _ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error
  ) {
    MainActor.assumeIsolated { setConnecting(webView, false) }
  }

  private func setConnecting(_ webView: WKWebView, _ on: Bool) {
    guard let id = live.first(where: { $0.value === webView })?.key else { return }
    if on { connecting.insert(id) } else { connecting.remove(id) }
  }

  nonisolated func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
    MainActor.assumeIsolated {
      guard let url = webView.url?.absoluteString,
        let id = live.first(where: { $0.value === webView })?.key
      else { return }
      onPageLoaded?(url, webView.title ?? "", id)
      Favicons.shared.pageLoaded(webView, tab: id)
    }
  }

  /// A long press on the page: the system's items, then the extensions'
  /// context menu items (chrome.contextMenus) for that spot.
  nonisolated func webView(
    _ webView: WKWebView, contextMenuConfigurationFor elementInfo: WKContextMenuElementInfo
  ) async -> UIContextMenuConfiguration? {
    await MainActor.run {
      guard let id = live.first(where: { $0.value === webView })?.key,
        let items = extensions?.menuItems(forTab: id), !items.isEmpty
      else { return nil }
      return UIContextMenuConfiguration(identifier: nil, previewProvider: nil) { suggested in
        UIMenu(children: suggested + [UIMenu(options: .displayInline, children: items)])
      }
    }
  }

  nonisolated func webView(
    _ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
    for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures
  ) -> WKWebView? {
    if let url = navigationAction.request.url {
      MainActor.assumeIsolated { openInNewTab?(url) }
    }
    return nil
  }
}

/// Forwards script messages without the content controller retaining the
/// tab manager.
final class MessageProxy: NSObject, WKScriptMessageHandler {
  weak var target: TabManager?
  init(_ target: TabManager) { self.target = target }

  func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
    MainActor.assumeIsolated {
      guard let target, let view = message.webView,
        let id = target.live.first(where: { $0.value === view })?.key
      else { return }
      if message.name == AutoPiP.handler {
        if let state = message.body as? String, state.hasPrefix("state:") {
          if state == "state:playing" { target.playingVideo.insert(id) } else { target.playingVideo.remove(id) }
          return
        }
        if let scroll = message.body as? String, scroll.hasPrefix("scroll:") {
          target.onPictureInPictureScroll?(id, scroll == "scroll:out")
          return
        }
        if let log = message.body as? String, log.hasPrefix("log:") {
          // The window closed: a parked view leaves the lot.
          if log == "log:mode inline" { DispatchQueue.main.async { AutoPiP.unpark(view) } }
          #if DEBUG
            print("AutoPiP: page: \(log.dropFirst(4))")
          #endif
          return
        }
        target.onPictureInPictureRestore?(id)
        return
      }
      target.onPasswordMessage?(message.body, id, view.url)
    }
  }
}
