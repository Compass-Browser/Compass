// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import SwiftUI
import UIKit
import WebKit

/// Reader mode (docs/design/reader-mode.md, "iOS"), with the desktop's
/// shared package (common/reader/, bundled):
/// - `Readability-readerable.js` and `reader.js` `check()` say whether a
///   page reads as an article; `Readability.js` and `reader.js` `parse()`
///   take the article out of it. Both run in Compass's own content world,
///   never the page's, so the page can't see or change them.
/// - The article shows over the tab in a web view of its own: the shared
///   `reader.html`, `reader.css` and `reader_view.js` (the desktop's look,
///   toolbar, settings panel and Listen), served under `compass-reader://`
///   with the desktop's Content Security Policy, so only those scripts run.
///   The tab's page waits underneath, untouched.
/// Nothing leaves the device; it works in Incognito.
@MainActor @Observable
final class ReaderMode {
  /// Its own world: the page never sees the reader's scripts.
  static let world = WKContentWorld.world(name: "CompassReader")

  /// Tabs whose page reads as an article, by the URL that was checked.
  private(set) var readerable: [String: URL] = [:]
  /// The look (UserDefaults, the desktop's key and JSON): reader_view.js
  /// reads it as the page loads and reports changes.
  var settings: ReaderSettings {
    didSet { if settings != oldValue { settings.save(to: .standard) } }
  }
  /// The URL each tab was last checked at.
  @ObservationIgnored private var checked: [String: URL] = [:]

  init() {
    settings = ReaderSettings(defaults: .standard)
  }

  /// Whether the tab's page, as it is now, reads as an article.
  func isReaderable(_ id: String, _ view: WKWebView?) -> Bool {
    guard let url = view?.url else { return false }
    return readerable[id] == url
  }

  private func forget(_ id: String) {
    if readerable[id] != nil { readerable[id] = nil }
  }

  /// A page loaded: does it read as an article?
  func check(_ id: String, _ view: WKWebView) {
    guard let url = view.url, ReaderScripts.isWebPage(url) else {
      checked[id] = nil
      return forget(id)
    }
    checked[id] = url
    Task {
      let yes = await ReaderScripts.check(view)
      // Still the same page?
      guard view.url == url else { return }
      if yes { readerable[id] = url } else { forget(id) }
    }
  }

  /// The tab's URL or title changed. Another URL: not an article until
  /// it's checked, when it has loaded (`check`), or for a change within
  /// the page (a single-page app's pushState) a second later, as on the
  /// desktop.
  func pageChanged(_ id: String, _ view: WKWebView) {
    guard let url = view.url, checked[id] != url else { return }
    checked[id] = nil
    forget(id)
    Task {
      try? await Task.sleep(for: .seconds(1))
      guard view.url == url, !view.isLoading, checked[id] != url else { return }
      check(id, view)
    }
  }
}

/// The shared scripts, run in a tab's page in the reader's world.
@MainActor
enum ReaderScripts {
  static func bundled(_ name: String, _ ext: String) -> String {
    guard let url = Bundle.main.url(forResource: name, withExtension: ext),
      let text = try? String(contentsOf: url, encoding: .utf8)
    else { return "" }
    return text
  }

  private static let readability = bundled("Readability", "js")
  private static let readerable = bundled("Readability-readerable", "js")
  private static let reader = bundled("reader", "js")

  static func isWebPage(_ url: URL) -> Bool { ["http", "https"].contains(url.scheme?.lowercased() ?? "") }

  /// Defines the scripts' globals in the world (each defines them once
  /// per world; `__compassReader` finds Readability when it's called).
  /// Not through `callAsyncJavaScript`, whose body is a function: its
  /// declarations wouldn't be global.
  private static func define(_ sources: [String], in view: WKWebView) async -> Bool {
    let source = sources.joined(separator: "\n;\n") + "\n;true"
    return (try? await view.evaluateJavaScript(source, in: nil, contentWorld: ReaderMode.world)) as? Bool == true
  }

  /// reader.js `check()`: Readability-readerable's `isProbablyReaderable`.
  static func check(_ view: WKWebView) async -> Bool {
    guard !reader.isEmpty, await define([readerable, reader], in: view) else { return false }
    let value = try? await view.callAsyncJavaScript(
      "return JSON.stringify(__compassReader.check());", arguments: [:], in: nil, contentWorld: ReaderMode.world)
    let result = ReaderResult(json: value as? String)
    return result.ok && result.readerable
  }

  /// reader.js `parse()`: the article, as JSON for the reader view (an
  /// error in the same form when there's none).
  static func parse(_ view: WKWebView) async -> ReaderResult {
    guard !reader.isEmpty, await define([readability, readerable, reader], in: view) else {
      return ReaderResult(json: ReaderResult.failure("This page has no article."))
    }
    let value = try? await view.callAsyncJavaScript(
      "return JSON.stringify(__compassReader.parse({}));", arguments: [:], in: nil, contentWorld: ReaderMode.world)
    return ReaderResult(json: value as? String)
  }
}

/// Serves the reader page's files (ReaderPageFiles) from the bundle, with
/// the Content Security Policy; nothing else.
final class ReaderSchemeHandler: NSObject, WKURLSchemeHandler {
  func webView(_ webView: WKWebView, start task: any WKURLSchemeTask) {
    MainActor.assumeIsolated { serve(task) }
  }

  @MainActor private func serve(_ task: any WKURLSchemeTask) {
    guard let url = task.request.url, url.host() == ReaderPageFiles.host,
      let file = ReaderPageFiles.files[url.path()],
      let source = Bundle.main.url(forResource: file.name, withExtension: file.ext),
      var data = try? Data(contentsOf: source)
    else {
      task.didFailWithError(URLError(.fileDoesNotExist))
      return
    }
    if file.type == "text/html" { data = Data(ReaderPageFiles.page(String(decoding: data, as: UTF8.self)).utf8) }
    let response = HTTPURLResponse(
      url: url, statusCode: 200, httpVersion: "HTTP/1.1",
      headerFields: [
        "Content-Type": "\(file.type); charset=utf-8",
        "Content-Security-Policy": ReaderPageFiles.contentSecurityPolicy,
        "Referrer-Policy": "no-referrer",
        "Cache-Control": "no-store",
      ])!
    task.didReceive(response)
    task.didReceive(data)
    task.didFinish()
  }

  func webView(_ webView: WKWebView, stop task: any WKURLSchemeTask) {}
}

/// The reader over one tab: the shared reader page in a web view of its
/// own, showing the article reader.js took from the tab's page.
@MainActor
final class ReaderPage: NSObject, WKNavigationDelegate, Identifiable {
  static let handler = "compassReader"
  let tabID: String
  /// The page the article came from.
  let url: URL
  let webView: WKWebView
  /// The reader's × (or Esc): Reader closes.
  var onClose: (() -> Void)?
  /// A link tapped in the article: the tab loads it, and Reader closes.
  var onLink: ((URL) -> Void)?
  /// reader_view.js changed the look (Aa).
  var onSettings: ((ReaderSettings) -> Void)?
  private let article: ReaderResult
  private var rendered = false
  private var closing = false

  /// `dataStore` is the tab's (Incognito's stays in memory): the article's
  /// images come from the page's site.
  init(tabID: String, url: URL, article: ReaderResult, dataStore: WKWebsiteDataStore, settings: ReaderSettings) {
    self.tabID = tabID
    self.url = url
    self.article = article
    let configuration = WKWebViewConfiguration()
    configuration.websiteDataStore = dataStore
    configuration.setURLSchemeHandler(ReaderSchemeHandler(), forURLScheme: ReaderPageFiles.scheme)
    configuration.dataDetectorTypes = []
    let controller = configuration.userContentController
    controller.addUserScript(WKUserScript(
      source: ReaderPageFiles.hostScript(settings: settings, handler: Self.handler),
      injectionTime: .atDocumentStart, forMainFrameOnly: true))
    webView = WKWebView(frame: .zero, configuration: configuration)
    webView.isOpaque = false
    webView.backgroundColor = .clear
    webView.scrollView.backgroundColor = .clear
    webView.isFindInteractionEnabled = true
    webView.accessibilityIdentifier = "ReaderPage"
    // Shown once the article is in (the theme's color until then).
    webView.alpha = 0
    super.init()
    // Weakly: the controller keeps its handlers.
    controller.add(WeakMessageHandler(self), name: Self.handler)
    webView.navigationDelegate = self
    webView.load(URLRequest(url: ReaderPageFiles.pageURL))
  }

  /// The reader goes: Listen stops, and the page with it.
  func close() {
    closing = true
    webView.stopLoading()
    webView.configuration.userContentController.removeScriptMessageHandler(forName: Self.handler)
    webView.evaluateJavaScript("globalThis.speechSynthesis && speechSynthesis.cancel(); true", in: nil, in: .page) {
      [webView] _ in webView.loadHTMLString("", baseURL: nil)
    }
  }

  // MARK: The page

  nonisolated func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
    MainActor.assumeIsolated { render() }
  }

  /// The article into the page (reader_view.js sanitizes it again).
  private func render() {
    guard !rendered, webView.url?.scheme == ReaderPageFiles.scheme else { return }
    rendered = true
    webView.callAsyncJavaScript(
      "compassReaderView.render(JSON.parse(article));", arguments: ["article": article.json], in: nil, in: .page
    ) { [webView] _ in
      UIView.animate(withDuration: 0.15) { webView.alpha = 1 }
    }
  }

  /// From reader_view.js, through `compassReaderHost`.
  fileprivate func received(_ message: WKScriptMessage) {
    guard message.frameInfo.isMainFrame, message.frameInfo.request.url?.scheme == ReaderPageFiles.scheme,
      let body = message.body as? [String: Any]
    else { return }
    if body["close"] as? Bool == true { onClose?() }
    if let settings = body["settings"] { onSettings?(ReaderSettings(settings)) }
  }

  nonisolated func webView(
    _ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
    decisionHandler: @escaping @MainActor (WKNavigationActionPolicy) -> Void
  ) {
    MainActor.assumeIsolated {
      guard let target = action.request.url else { return decisionHandler(.cancel) }
      // Images and the like aren't navigations; this is a frame or a link.
      switch ReaderLink.decide(target, page: url) {
      case .stay:
        decisionHandler(action.targetFrame?.isMainFrame == true && (!rendered || closing) ? .allow : .cancel)
      case .leave:
        decisionHandler(.cancel)
        onClose?()
      case .openInTab(let link):
        decisionHandler(.cancel)
        onLink?(link)
      case .system(let link):
        decisionHandler(.cancel)
        UIApplication.shared.open(link)
      case .ignore:
        decisionHandler(.cancel)
      }
    }
  }

  /// The reader page crashed: show it again.
  nonisolated func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
    MainActor.assumeIsolated {
      rendered = false
      webView.load(URLRequest(url: ReaderPageFiles.pageURL))
    }
  }
}

/// A script message handler that doesn't keep its target.
private final class WeakMessageHandler: NSObject, WKScriptMessageHandler {
  weak var target: ReaderPage?
  init(_ target: ReaderPage) { self.target = target }

  func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
    MainActor.assumeIsolated { target?.received(message) }
  }
}

extension AppModel {
  /// The reader over the shown tab, if it's in Reader.
  var shownReader: ReaderPage? {
    guard let tab = shownTab else { return nil }
    return readers[tab.id]
  }

  /// Whether the shown tab's page reads as an article.
  var canShowReader: Bool {
    let _ = services.reader.readerable
    guard let tab = shownTab else { return false }
    return services.reader.isReaderable(tab.id, tabs.live[tab.id])
  }

  /// The Reader button, its menu items and ⌘⌥R: the shown tab's article,
  /// or back to the page. The shortcut works on any web page, as on the
  /// desktop: one with no article says so.
  func toggleReader() {
    guard let tab = shownTab else { return }
    if readers[tab.id] != nil { return closeReader(tab.id) }
    guard let view = tabs.live[tab.id], let url = view.url, ReaderScripts.isWebPage(url) else { return }
    let dataStore = tabs.dataStore(of: tab)
    let id = tab.id
    Task { [weak self, weak view] in
      guard let view else { return }
      let article = await ReaderScripts.parse(view)
      guard let self, view.url == url, readers[id] == nil else { return }
      let page = ReaderPage(tabID: id, url: url, article: article, dataStore: dataStore, settings: services.reader.settings)
      page.onClose = { [weak self] in self?.closeReader(id) }
      page.onLink = { [weak self, weak view] link in
        self?.closeReader(id)
        view?.load(URLRequest(url: link))
      }
      page.onSettings = { [weak self] in self?.services.reader.settings = $0 }
      // Readers of tabs that have gone.
      for gone in readers.keys where store.node(id: gone) == nil { closeReader(gone) }
      withAnimation(.easeInOut(duration: 0.2)) { readers[id] = page }
    }
  }

  /// The tab leaves Reader: its page is there underneath, as it was.
  func closeReader(_ id: String) {
    guard let page = readers[id] else { return }
    withAnimation(.easeInOut(duration: 0.2)) { readers[id] = nil }
    page.close()
  }
}

/// The reader over the tab, laid out as the page is (under the status bar,
/// over the bar's inset): reader_view.js's toolbar floats at its top.
struct ReaderScreen: View {
  @Environment(AppModel.self) private var app
  let page: ReaderPage
  var topInset: CGFloat = 0
  var bottomInset: CGFloat = 0
  var obscuredBottom: CGFloat = 0

  var body: some View {
    WebView(webView: page.webView, bottomInset: bottomInset, topInset: topInset, obscuredBottom: obscuredBottom)
      // reader.css's background while the page loads.
      .background(ReaderLook.background(app.services.reader.settings.theme))
  }
}

/// reader.css's backgrounds, for behind the reader page.
enum ReaderLook {
  static func background(_ theme: String) -> Color {
    let light = UIColor(red: 0xfb / 255, green: 0xfb / 255, blue: 0xfa / 255, alpha: 1)
    let dark = UIColor(red: 0x1c / 255, green: 0x1c / 255, blue: 0x1e / 255, alpha: 1)
    switch theme {
    case "light": return Color(light)
    case "dark": return Color(dark)
    case "sepia": return Color(red: 0xf6 / 255, green: 0xef / 255, blue: 0xe2 / 255)
    default: return Color(UIColor { $0.userInterfaceStyle == .dark ? dark : light })
    }
  }
}
