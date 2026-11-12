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
  /// Session history (`WKWebView.interactionState`) for tabs that open
  /// again on an undo close: the next web view made for the tab gets it,
  /// back and forward included, instead of loading its URL.
  @ObservationIgnored var restoring: [String: Any] = [:]
  /// A long press's preview page (Peek), until its menu commits or ends.
  private let preview = PreviewSlot()
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
  /// A page to show in Peek's card: (its web view, the tab it came from).
  var onPeek: ((WKWebView, String) -> Void)?
  /// A link's long-press item: (what to do, the link, the tab it's in).
  var onLinkAction: ((LinkAction, URL, String) -> Void)?

  /// The long-press menu's items for a web link, after Peek.
  enum LinkAction {
    /// A new Today entry of the tab's space, shown.
    case newTab
    /// The same, not shown.
    case background
    /// A new incognito tab, shown.
    case incognito
    /// A little window (Peek.swift), of the tab's profile.
    case littleWindow
    /// A new Pinned entry of the tab's space.
    case pin
  }
  /// A page that isn't a tab (a peeked one) closed itself (`window.close()`).
  var onPageClosed: ((WKWebView) -> Void)?
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
    let view = makeWebView(for: node, url: URL(string: node.url))
    register(view, as: node.id)
    if let state = restoring.removeValue(forKey: node.id) {
      view.interactionState = state
    } else if let url = URL(string: node.url), !node.url.isEmpty {
      view.load(URLRequest(url: url))
    }
    return view
  }

  /// A web view as a tab of `node`'s would have it (its profile's website
  /// data, the scripts, the extensions, the filter lists for `url`), not
  /// loaded and not yet anyone's tab. Peek's pages start here
  /// (docs/design/ios.md, "Peek").
  ///
  /// `configuration`: WebKit's own, for a page a tab opens
  /// (`createWebViewWith`). It's the opener's, scripts and data store
  /// included, and has to be used as given for the page to keep its opener.
  func makeWebView(
    for node: SidebarNode, url: URL?, configuration given: WKWebViewConfiguration? = nil
  ) -> WKWebView {
    makeWebView(profile: store.profile(of: node), incognito: store.isIncognito(node), url: url, configuration: given)
  }

  /// The same for a page of `profile` that has no tab to come from (a link
  /// from another app, in a little window).
  func makeWebView(
    profile: String, incognito: Bool, url: URL?, configuration given: WKWebViewConfiguration? = nil
  ) -> WKWebView {
    let view: WKWebView
    if let given {
      view = WKWebView(frame: .zero, configuration: given)
    } else {
      let configuration = WKWebViewConfiguration()
      // The home profile's spaces (and favorites) use the default store.
      // Incognito: one in-memory store for the app's run (cookies, storage
      // and cache never touch the disk).
      configuration.websiteDataStore = incognito
        ? incognitoDataStore : dataStore(for: store.isHome(profile) ? "" : profile)
      configuration.allowsInlineMediaPlayback = true
      configuration.allowsPictureInPictureMediaPlayback = true
      configuration.userContentController.addUserScript(PasswordScript.userScript)
      configuration.userContentController.addUserScript(DesktopViewport.script)
      configuration.userContentController.add(
        MessageProxy(self), contentWorld: PasswordScript.world, name: PasswordScript.handler)
      if let callScript = AutoPiP.callScript {
        configuration.userContentController.addUserScript(callScript)
      }
      configuration.userContentController.addUserScript(AutoPiP.inlineScript)
      configuration.userContentController.addUserScript(AutoPiP.script)
      // Claude's read_console (ClaudeTools.swift).
      configuration.userContentController.addUserScript(ConsoleCapture.script)
      configuration.userContentController.add(MessageProxy(self), contentWorld: AutoPiP.world, name: AutoPiP.handler)
      view = WKWebView(
        frame: .zero,
        configuration: extensions?.configure(configuration, for: url) ?? configuration)
    }
    view.allowsBackForwardNavigationGestures = true
    view.isFindInteractionEnabled = true
    applyContentBlocking(view, site: SiteSettings.site(url))
    view.isInspectable = true
    view.navigationDelegate = self
    view.uiDelegate = self
    return view
  }

  /// `view`, made by `makeWebView` and maybe loaded already (a peeked
  /// page), becomes the live tab of `node` as it is: no reload, its history,
  /// scroll position and form state kept.
  func adopt(_ view: WKWebView, as node: SidebarNode) {
    if let old = live[node.id], old !== view { unload(node.id) }
    register(view, as: node.id)
    changed(node.id, view)
  }

  private func register(_ view: WKWebView, as id: String) {
    observe(view, as: id)
    live[id] = view
    bump(id)
    // Before it loads, so extensions know the tab its pages talk from (a
    // tab restored from the sidebar was never announced: 1Password's
    // background then found no tab for its messages).
    extensions?.tabOpened(id)
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
  /// Incognito's website data, in memory; a new one after "Close All
  /// Incognito Tabs".
  private(set) var incognitoDataStore = WKWebsiteDataStore.nonPersistent()

  /// Incognito ends: its tabs unload and its website data goes.
  func resetIncognito() {
    for id in live.keys where store.node(id: id).map(store.isIncognito) ?? false { unload(id) }
    incognitoDataStore = .nonPersistent()
  }

  /// Whether a live tab is incognito.
  func isIncognito(_ id: String) -> Bool { store.node(id: id).map(store.isIncognito) ?? false }

  /// Every profile's website data store: the home profile's (the default)
  /// and each other profile's in the sidebar.
  func allDataStores() -> [WKWebsiteDataStore] {
    let profiles = Set(store.spaces.map(\.profileUUID).filter { !store.isHome($0) })
    return [.default()] + profiles.sorted().map(dataStore(for:))
  }

  /// The website data a node's pages use: its profile's (live folders
  /// read their cookies from it).
  func dataStore(of node: SidebarNode) -> WKWebsiteDataStore {
    if store.isIncognito(node) { return incognitoDataStore }
    let profile = store.profile(of: node)
    return dataStore(for: store.isHome(profile) ? "" : profile)
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
      // Incognito's sites keep their icons in memory only (Favicons).
      if navigationAction.targetFrame?.isMainFrame != false, let host = navigationAction.request.url?.host(),
        let id = live.first(where: { $0.value === webView })?.key, isIncognito(id)
      {
        Favicons.shared.markPrivate(host)
      }
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

  /// HTTP authentication (Basic, Digest): WKWebView shows no prompt of its
  /// own (without this, the server's 401 page shows), so Compass asks for
  /// the user name and password. Anything else (server trust, client
  /// certificates) gets the system's handling.
  nonisolated func webView(
    _ webView: WKWebView, didReceive challenge: URLAuthenticationChallenge,
    completionHandler: @escaping @MainActor (URLSession.AuthChallengeDisposition, URLCredential?) -> Void
  ) {
    let method = challenge.protectionSpace.authenticationMethod
    guard method == NSURLAuthenticationMethodHTTPBasic || method == NSURLAuthenticationMethodHTTPDigest else {
      return MainActor.assumeIsolated { completionHandler(.performDefaultHandling, nil) }
    }
    // A saved credential, tried once.
    if challenge.previousFailureCount == 0, let saved = challenge.proposedCredential, saved.hasPassword {
      return MainActor.assumeIsolated { completionHandler(.useCredential, saved) }
    }
    let space = challenge.protectionSpace
    let failed = challenge.previousFailureCount > 0
    MainActor.assumeIsolated {
      // WebKit waits on the answer: exactly once, and a cancel when the
      // alert couldn't be shown.
      final class Once: @unchecked Sendable {
        var handler: (@MainActor (URLSession.AuthChallengeDisposition, URLCredential?) -> Void)?
      }
      let once = Once()
      once.handler = completionHandler
      let answer: @MainActor @Sendable (URLSession.AuthChallengeDisposition, URLCredential?) -> Void = {
        disposition, credential in
        let handler = once.handler
        once.handler = nil
        handler?(disposition, credential)
      }
      var message = "\(space.host) requires a user name and password."
      if let realm = space.realm, !realm.isEmpty { message += "\n\u{201C}\(realm)\u{201D}" }
      if space.receivesCredentialSecurely == false { message += "\nYour login information will be sent insecurely." }
      if failed { message = "The user name or password was incorrect.\n" + message }
      let alert = UIAlertController(title: "Sign In", message: message, preferredStyle: .alert)
      alert.addTextField {
        $0.placeholder = "User Name"
        $0.textContentType = .username
        $0.autocapitalizationType = .none
        $0.autocorrectionType = .no
        $0.text = challenge.proposedCredential?.user
      }
      alert.addTextField {
        $0.placeholder = "Password"
        $0.textContentType = .password
        $0.isSecureTextEntry = true
      }
      alert.addAction(UIAlertAction(title: "Cancel", style: .cancel) { _ in
        answer(.cancelAuthenticationChallenge, nil)
      })
      alert.addAction(UIAlertAction(title: "Sign In", style: .default) { [weak alert] _ in
        let user = alert?.textFields?[0].text ?? ""
        let password = alert?.textFields?[1].text ?? ""
        answer(.useCredential, URLCredential(user: user, password: password, persistence: .forSession))
      })
      guard Presenter.present(alert) else { return answer(.cancelAuthenticationChallenge, nil) }
      // UIKit drops a presentation over one that's still animating.
      Task { @MainActor in
        try? await Task.sleep(for: .seconds(2))
        if alert.presentingViewController == nil { answer(.cancelAuthenticationChallenge, nil) }
      }
    }
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
      Favicons.shared.pageLoaded(webView, tab: id, private: isIncognito(id))
    }
  }

  /// A long press on the page: the system's items, then the extensions'
  /// context menu items (chrome.contextMenus) for that spot. On a web link,
  /// the preview is the page itself, loading in a web view of the tab's
  /// profile: tapping it (or Peek) shows that same page in Peek's card.
  ///
  /// The completion-handler form, not the async one: WebKit continues the
  /// menu interaction (UIKit view work) on whatever thread the completion
  /// runs on, and a nonisolated async method finishes on a background
  /// executor, which crashed on long presses (TestFlight 20261110.2150).
  /// On the main actor, with WebKit's `@MainActor` completion handler: with
  /// a plain one the method only "nearly matches" the protocol's and WebKit
  /// never calls it.
  func webView(
    _ webView: WKWebView, contextMenuConfigurationForElement elementInfo: WKContextMenuElementInfo,
    completionHandler: @escaping @MainActor (UIContextMenuConfiguration?) -> Void
  ) {
    let configuration = { () -> UIContextMenuConfiguration? in
      guard let id = live.first(where: { $0.value === webView })?.key else { return nil }
      let items = extensions?.menuItems(forTab: id) ?? []
      let link = elementInfo.linkURL.flatMap { Self.isWeb($0) ? $0 : nil }
      let node = store.node(id: id)
      let peeks = link != nil && onPeek != nil && node != nil
      let acts = link != nil && onLinkAction != nil && node != nil
      // An incognito tab's links stay incognito: no Open in Incognito (it
      // is already), and no little window or Pin, which would put them in a
      // normal space.
      let incognito = node.map(store.isIncognito) ?? false
      guard peeks || acts || !items.isEmpty else { return nil }
      let size = webView.bounds.size
      let preview: UIContextMenuContentPreviewProvider? =
        peeks ? { [weak self] in link.flatMap { self?.peekPreview($0, from: id, size: size) } } : nil
      return UIContextMenuConfiguration(identifier: nil, previewProvider: preview) { [weak self] suggested in
        // Peek and the link's places first, then the system's items (Open
        // Link, Copy Link, Share…), then the extensions'.
        var ours: [UIMenuElement] = []
        if peeks, let link {
          ours.append(
            UIAction(title: "Peek", image: UIImage(systemName: "eye")) { [weak self] _ in
              self?.peek(link, from: id)
            })
        }
        if acts, let link {
          var actions: [(String, String, LinkAction)] = [
            ("Open in New Tab", "plus.square.on.square", .newTab),
            ("Open in Background", "square.on.square.dashed", .background),
          ]
          if !incognito {
            actions += [
              ("Open in Incognito", "eyeglasses", .incognito),
              ("Open in Little Window", "macwindow", .littleWindow),
              ("Pin", "pin", .pin),
            ]
          }
          for (title, symbol, action) in actions {
            ours.append(
              UIAction(title: title, image: UIImage(systemName: symbol)) { [weak self] _ in
                self?.onLinkAction?(action, link, id)
              })
          }
        }
        var children = suggested
        if !ours.isEmpty { children.insert(UIMenu(options: .displayInline, children: ours), at: 0) }
        if !items.isEmpty { children.append(UIMenu(options: .displayInline, children: items)) }
        return UIMenu(children: children)
      }
    }()
    completionHandler(configuration)
  }

  /// The preview was tapped: its page goes on to Peek's card, as it is.
  func webView(
    _ webView: WKWebView, contextMenuForElement elementInfo: WKContextMenuElementInfo,
    willCommitWithAnimator animator: UIContextMenuInteractionCommitAnimating
  ) {
    guard let preview = self.preview.take() else { return }
    animator.preferredCommitStyle = .pop
    animator.addCompletion { [weak self] in self?.onPeek?(preview.page, preview.source) }
  }

  /// The menu closed without the preview tapped: its page goes (unless
  /// Peek, from the menu, already took it).
  func webView(_ webView: WKWebView, contextMenuDidEndForElement elementInfo: WKContextMenuElementInfo) {
    // After the menu's action, which runs once the menu is gone.
    DispatchQueue.main.async { [weak self] in _ = self?.preview.take() }
  }

  /// Peek from the long-press menu: the preview's page if it was this
  /// link's, else a new one.
  private func peek(_ url: URL, from source: String) {
    guard let node = store.node(id: source) else { return }
    let page: WKWebView
    if let preview = self.preview.take(), preview.url == url, preview.source == source {
      page = preview.page
    } else {
      page = makeWebView(for: node, url: url)
      page.load(URLRequest(url: url))
    }
    onPeek?(page, source)
  }

  private func peekPreview(_ url: URL, from source: String, size: CGSize) -> UIViewController? {
    guard let node = store.node(id: source) else { return nil }
    let page = makeWebView(for: node, url: url)
    page.load(URLRequest(url: url))
    preview.value = PeekPreview(page: page, url: url, source: source)
    let controller = PeekPreviewController(page: page)
    if size.width > 0, size.height > 0 {
      controller.preferredContentSize = CGSize(width: size.width, height: (size.height * 0.75).rounded())
    }
    return controller
  }

  nonisolated private static func isWeb(_ url: URL) -> Bool {
    let scheme = url.scheme?.lowercased()
    return scheme == "http" || scheme == "https"
  }

  /// A page wants a new tab or window (`target=_blank`, `window.open()`):
  /// from a pinned tab or a favorite it peeks, so the pinned app stays put
  /// (as on the desktop: docs/design/peek.md); from anywhere else it's a
  /// new Today entry. WebKit only asks on a user's tap (iOS blocks pop-ups
  /// that aren't).
  nonisolated func webView(
    _ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
    for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures
  ) -> WKWebView? {
    MainActor.assumeIsolated { () -> WKWebView? in
      let url = navigationAction.request.url
      if onPeek != nil, let id = live.first(where: { $0.value === webView })?.key, let node = store.node(id: id),
        node.section == .pinned || node.section == .favorites, url.map(Self.isWeb) ?? true
      {
        // WebKit's configuration, so the page keeps its opener; WebKit loads
        // the request in it.
        let page = makeWebView(for: node, url: url, configuration: configuration)
        onPeek?(page, id)
        return page
      }
      if let url { openInNewTab?(url) }
      return nil
    }
  }

  /// `window.close()`: a peeked page's card closes (tabs stay, as before).
  func webViewDidClose(_ webView: WKWebView) {
    guard !live.values.contains(where: { $0 === webView }) else { return }
    onPageClosed?(webView)
  }

  // MARK: Peek's preview

  /// The page a long press is previewing, until the menu commits or ends.
  struct PeekPreview {
    let page: WKWebView
    let url: URL
    let source: String
  }

  /// Holds the preview's page (a box, so the delegate methods can take it
  /// without being observed).
  final class PreviewSlot {
    var value: PeekPreview?
    func take() -> PeekPreview? {
      defer { value = nil }
      return value
    }
  }
}

/// A long press's preview: the linked page, not yet anyone's tab.
private final class PeekPreviewController: UIViewController {
  let page: WKWebView

  init(page: WKWebView) {
    self.page = page
    super.init(nibName: nil, bundle: nil)
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError() }

  override func loadView() {
    view = UIView()
    view.backgroundColor = .systemBackground
    page.frame = view.bounds
    page.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    view.addSubview(page)
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
          AutoPiP.note("page \(id.prefix(8)): \(state.dropFirst(6))")
          return
        }
        if let scroll = message.body as? String, scroll.hasPrefix("scroll:") {
          AutoPiP.note("page \(id.prefix(8)): \(scroll)")
          target.onPictureInPictureScroll?(id, scroll == "scroll:out")
          return
        }
        if let log = message.body as? String, log.hasPrefix("log:") {
          // The window closed: a parked view leaves the lot.
          if log == "log:mode inline" { DispatchQueue.main.async { AutoPiP.unpark(view) } }
          AutoPiP.note("page \(id.prefix(8)): \(log.dropFirst(4))")
          return
        }
        target.onPictureInPictureRestore?(id)
        return
      }
      target.onPasswordMessage?(message.body, id, view.url)
    }
  }
}
