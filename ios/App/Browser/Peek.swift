// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI
import WebKit

/// A page in Peek's card (docs/design/ios.md, "Peek"): a link looked at
/// without opening a tab. It's a web view of the source tab's profile that
/// isn't anyone's tab: closing the card lets it go; Open makes it a Today
/// entry as it is (`TabManager.adopt`), no reload.
@MainActor
@Observable
final class Peek: Identifiable {
  let page: WKWebView
  /// The tab the link was in.
  let source: String
  /// Where Open puts the page: the source's space (for a favorite, the
  /// window's space if it's of the same profile).
  let spaceID: String?
  /// The page's profile: Open can't put it in another profile's space.
  let profile: String
  private(set) var title = ""
  private(set) var url: URL?
  @ObservationIgnored private var observations: [NSKeyValueObservation] = []

  init(page: WKWebView, source: String, spaceID: String?, profile: String) {
    self.page = page
    self.source = source
    self.spaceID = spaceID
    self.profile = profile
    title = page.title ?? ""
    url = page.url
    observations = [
      page.observe(\.title, options: [.new]) { [weak self] page, _ in
        MainActor.assumeIsolated { self?.title = page.title ?? "" }
      },
      page.observe(\.url, options: [.new]) { [weak self] page, _ in
        MainActor.assumeIsolated { self?.url = page.url }
      },
    ]
  }

  /// The pill's text: the title, or only the host until there is one.
  var heading: String { title.isEmpty ? host : title }
  var host: String {
    guard let host = url?.host() else { return url?.absoluteString ?? "" }
    return host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
  }
}

extension AppModel {
  /// Shows `page` (from the tab `source`) in Peek's card, over whatever this
  /// window shows. The sheets go: SwiftUI shows one at a time.
  func showPeek(_ page: WKWebView, from source: String) {
    guard let node = store.node(id: source) else { return }
    let profile = store.profile(of: node)
    dismissOverlays()
    showingClaude = false
    peek = Peek(page: page, source: source, spaceID: peekSpace(profile: profile, of: node)?.id, profile: profile)
  }

  /// The card closes and its page goes.
  func closePeek() {
    peek?.page.stopLoading()
    peek = nil
  }

  /// Open (⌘O): the card's page becomes the newest Today entry of its space
  /// and is shown, history, scroll position and form state kept.
  func openPeek() {
    guard let peek else { return }
    self.peek = nil
    let space = peek.spaceID.flatMap(store.space(id:)) ?? peekSpace(profile: peek.profile, of: nil)
    let page = peek.page
    guard let space,
      let tab = store.addTab(url: page.url?.absoluteString ?? "", title: page.title ?? "", to: space.today)
    else { return }
    tabs.adopt(page, as: tab)
    services.claim(tab.id, for: self)
    show(tab)
  }

  /// A space of `profile` for a peeked page: `node`'s own, else this
  /// window's (a favorite's page goes where the user is), else the
  /// profile's first.
  private func peekSpace(profile: String, of node: SidebarNode?) -> Space? {
    if let node, let space = store.space(of: node) { return space }
    if let space = selectedSpace, !space.isIncognito, store.sameProfile(space.profileUUID, profile) { return space }
    return store.spaces.first { store.sameProfile($0.profileUUID, profile) }
  }
}

/// Peek's card: the page with its title and host, Open and close. A sheet:
/// on a phone it fills the screen but for the top (swipe down closes it);
/// on an iPad it's a page-sized card over the dimmed window (a tap outside
/// closes it).
struct PeekCard: View {
  @Environment(AppModel.self) private var app
  let peek: Peek

  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: 10) {
        VStack(alignment: .leading, spacing: 1) {
          Text(peek.heading).font(.subheadline.weight(.semibold)).lineLimit(1)
          if !peek.title.isEmpty, !peek.host.isEmpty {
            Text(peek.host).font(.caption).foregroundStyle(.secondary).lineLimit(1)
          }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        Button { app.openPeek() } label: {
          Label("Open", systemImage: "arrow.up.forward.square").font(.subheadline.weight(.semibold))
        }
        .buttonStyle(.borderedProminent)
        .buttonBorderShape(.capsule)
        .accessibilityHint("Opens the page as a new tab")
        Button { app.closePeek() } label: {
          Image(systemName: "xmark.circle.fill")
            .font(.title2)
            .symbolRenderingMode(.hierarchical)
            .foregroundStyle(.secondary)
        }
        .buttonStyle(.plain)
        // Esc, with a keyboard.
        .keyboardShortcut(.cancelAction)
        .accessibilityLabel("Close")
      }
      .padding(.horizontal, 16)
      .padding(.top, 18)
      .padding(.bottom, 10)
      WebView(webView: peek.page)
        .ignoresSafeArea(edges: .bottom)
    }
    .presentationDetents([.large])
    .presentationDragIndicator(.visible)
    .peekSizing()
  }
}

extension View {
  /// On an iPad, the card is page sized rather than a narrow form sheet
  /// (iOS 18; before, the form sheet).
  @ViewBuilder fileprivate func peekSizing() -> some View {
    if #available(iOS 18.0, *) {
      presentationSizing(.page)
    } else {
      self
    }
  }
}
