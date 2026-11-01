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
  private var recent: [String] = []
  private var observations: [String: [NSKeyValueObservation]] = [:]
  /// Bumped when a live tab's state (loading, back/forward) changes.
  private(set) var revision = 0
  private let store: SpaceStore
  /// A link that wants a new window opens as a new Today entry.
  var openInNewTab: ((URL) -> Void)?
  /// A page finished loading (a history visit).
  var onPageLoaded: ((String, String) -> Void)?

  init(store: SpaceStore) {
    self.store = store
  }

  func isLive(_ node: SidebarNode) -> Bool { live[node.id] != nil }

  /// The web view of a tab entry, created and loaded on first use.
  func webView(for node: SidebarNode) -> WKWebView {
    if let view = live[node.id] {
      bump(node.id)
      return view
    }
    let configuration = WKWebViewConfiguration()
    configuration.websiteDataStore = dataStore(for: store.space(of: node)?.profileUUID ?? "")
    configuration.allowsInlineMediaPlayback = true
    configuration.allowsPictureInPictureMediaPlayback = true
    let view = WKWebView(frame: .zero, configuration: configuration)
    view.allowsBackForwardNavigationGestures = true
    view.isInspectable = true
    view.navigationDelegate = self
    view.uiDelegate = self
    let id = node.id
    observe(view, as: id)
    live[id] = view
    bump(id)
    if let url = URL(string: node.url), !node.url.isEmpty { view.load(URLRequest(url: url)) }
    return view
  }

  private func observe(_ view: WKWebView, as id: String) {
    observations[id] = [
      view.observe(\.url, options: [.new]) { [weak self] view, _ in
        Task { @MainActor in self?.changed(id, view) }
      },
      view.observe(\.title, options: [.new]) { [weak self] view, _ in
        Task { @MainActor in self?.changed(id, view) }
      },
      view.observe(\.estimatedProgress, options: [.new]) { [weak self] _, _ in
        Task { @MainActor in self?.revision += 1 }
      },
    ]
  }

  /// An entry got a new ID (kept after a remote close): its page follows.
  func rekey(_ old: String, to new: String) {
    guard let view = live.removeValue(forKey: old) else { return }
    observations[old] = nil
    observe(view, as: new)
    live[new] = view
    recent = recent.map { $0 == old ? new : $0 }
  }

  func unload(_ id: String) {
    observations[id] = nil
    live[id]?.stopLoading()
    live[id] = nil
    recent.removeAll { $0 == id }
  }

  /// Drops web views whose entries are gone (closed here or elsewhere).
  func prune() {
    for id in live.keys where store.node(id: id) == nil { unload(id) }
  }

  private func bump(_ id: String) {
    recent.removeAll { $0 == id }
    recent.append(id)
    while recent.count > Self.maxLive { unload(recent[0]) }
  }

  private func changed(_ id: String, _ view: WKWebView) {
    revision += 1
    guard let node = store.node(id: id) else { return }
    store.update(node, url: view.url?.absoluteString, title: view.title)
  }

  private var stores: [String: WKWebsiteDataStore] = [:]

  private func dataStore(for profile: String) -> WKWebsiteDataStore {
    guard !profile.isEmpty, let uuid = UUID(uuidString: profile) else { return .default() }
    if let store = stores[profile] { return store }
    let store = WKWebsiteDataStore(forIdentifier: uuid)
    stores[profile] = store
    return store
  }
}

extension TabManager: WKNavigationDelegate, WKUIDelegate {
  nonisolated func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
    MainActor.assumeIsolated {
      guard let url = webView.url?.absoluteString else { return }
      onPageLoaded?(url, webView.title ?? "")
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
