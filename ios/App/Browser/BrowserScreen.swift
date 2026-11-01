// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI
import WebKit

/// One entry, full screen, under a floating glass bar: back, the address
/// pill (opens the command bar; shows the load progress along its bottom),
/// forward when there's somewhere to go, and the sidebar. Swiping the bar
/// left or right moves between the space's entries; swiping it up opens the
/// sidebar. With no entry selected it's an empty page in the space's color.
struct BrowserScreen: View {
  @Environment(AppModel.self) private var app
  let node: SidebarNode?
  /// The bar's drag: nil until it picks an axis.
  @State private var dragAxis: Axis?
  /// The bar's height over the page: the 44 pt pill, its 5 pt padding and
  /// the 2 pt gap under it. A constant, not measured: feeding a measured
  /// height back into the web view's inset loops layout.
  static let barHeight: CGFloat = 44 + 2 * 5 + 2

  var body: some View {
    @Bindable var app = app
    VStack(spacing: 0) {
      if let node, node.isSplit {
        Picker("Pane", selection: $app.selectedPane) {
          ForEach(Array(node.children.enumerated()).filter { $0.element.isPhoneVisible }, id: \.offset) { index, pane in
            Text(pane.displayTitle).lineLimit(1).tag(index)
          }
        }
        .pickerStyle(.segmented)
        .padding(.horizontal)
        .padding(.vertical, 6)
      }
      Group {
        if let tab = app.shownTab {
          WebView(webView: app.tabs.webView(for: tab), bottomInset: Self.barHeight)
            .id(tab.id)
            // The page runs under the bar; WKWebView insets its content by
            // the safe area, so the page's end still scrolls clear of it.
            .ignoresSafeArea(edges: node?.isSplit == true ? .bottom : [.top, .bottom])
        } else {
          emptyPage
        }
      }
      .safeAreaInset(edge: .bottom, spacing: 0) {
        VStack(spacing: 8) {
          if let pending = app.passwords.pendingSave { saveBanner(pending) }
          bar
        }
        .animation(.snappy(duration: 0.25), value: app.passwords.pendingSave == nil)
      }
    }
  }

  private var emptyPage: some View {
    LinearGradient(
      colors: [(app.selectedSpace?.color.color ?? .blue).opacity(0.28), Color(.systemBackground)],
      startPoint: .top, endPoint: .bottom
    )
    .ignoresSafeArea(edges: .top)
  }

  private func saveBanner(_ p: PasswordManager.PendingSave) -> some View {
    HStack(spacing: 12) {
      Image(systemName: "key.fill").foregroundStyle(.secondary)
      VStack(alignment: .leading, spacing: 2) {
        Text(saveTitle(p)).font(.subheadline.weight(.semibold))
        Text("\(p.username.isEmpty ? "No username" : p.username) · \(URL(string: p.realm)?.host() ?? p.realm)")
          .font(.caption).foregroundStyle(.secondary).lineLimit(1)
      }
      Spacer()
      Menu {
        Button("Never for This Site") { app.passwords.neverSave(p) }
        Button("Not Now") { app.passwords.pendingSave = nil }
      } label: {
        Image(systemName: "ellipsis").padding(6)
      }
      Button(p.isUpdate ? "Update" : "Save") { app.passwords.save(p) }
        .buttonStyle(.borderedProminent)
    }
    .padding(12)
    .floatingGlass(in: RoundedRectangle(cornerRadius: 22, style: .continuous))
    .padding(.horizontal, 12)
    .transition(.move(edge: .bottom).combined(with: .opacity))
  }

  /// "Save to Work?" when the chain has several profiles.
  private func saveTitle(_ p: PasswordManager.PendingSave) -> String {
    if app.sync.profiles.count > 1, let name = app.sync.profileName(account: p.profile) {
      return p.isUpdate ? "Update in \(name)?" : "Save to \(name)?"
    }
    return p.isUpdate ? "Update password?" : "Save password?"
  }

  @ViewBuilder private var keyButton: some View {
    if let tab = app.shownTab, app.passwords.hasForm(tab.id), let view = app.tabs.live[tab.id] {
      let logins = app.passwords.matches(for: view.url, profile: app.accountProfile(ofTab: tab.id))
      if !logins.isEmpty {
        Menu {
          ForEach(logins) { login in
            Button(login.username.isEmpty ? login.host : login.username) { app.passwords.fill(login, in: view) }
          }
        } label: {
          BarIcon(symbol: "key.fill")
        }
        .accessibilityLabel("Fill password")
        .transition(.scale.combined(with: .opacity))
      }
    }
  }

  /// How far the shown page has loaded, or nil once it has.
  private var loadProgress: Double? {
    guard let tab = app.shownTab, let view = app.tabs.live[tab.id], view.isLoading else { return nil }
    return max(view.estimatedProgress, 0.05)
  }

  private var tint: Color {
    app.shownTab.flatMap { app.store.space(of: $0)?.color.color }
      ?? app.selectedSpace?.color.color ?? .accentColor
  }

  private var bar: some View {
    let _ = app.tabs.revision
    let view = app.shownTab.flatMap { app.tabs.live[$0.id] }
    let canBack = view?.canGoBack ?? false
    let canForward = view?.canGoForward ?? false
    return HStack(spacing: 2) {
      Button { view?.goBack() } label: { BarIcon(symbol: "chevron.backward") }
        .disabled(!canBack)
        .opacity(canBack ? 1 : 0.3)
        .accessibilityLabel("Back")
      addressPill
      if canForward {
        Button { view?.goForward() } label: { BarIcon(symbol: "chevron.forward") }
          .accessibilityLabel("Forward")
          .transition(.scale.combined(with: .opacity))
      }
      keyButton
      Button { app.showSidebar() } label: { BarIcon(symbol: "sidebar.left") }
        .accessibilityLabel("Sidebar")
    }
    .animation(.snappy(duration: 0.2), value: canForward)
    // The sidebar's neutral look, not the system tint.
    .buttonStyle(.plain)
    .foregroundStyle(.primary)
    .padding(5)
    .floatingGlass(in: Capsule())
    .padding(.horizontal, 12)
    .padding(.bottom, 2)
    // Over the buttons' own gestures (the address pill covers most of the
    // bar): a tap never moves 12 pt, so buttons still get taps.
    .highPriorityGesture(
      DragGesture(minimumDistance: 12, coordinateSpace: .global)
        .onChanged { value in
          let (dx, dy) = (value.translation.width, value.translation.height)
          if dragAxis == nil { dragAxis = abs(dx) > abs(dy) ? .horizontal : .vertical }
          if dragAxis == .vertical { app.sidebarPeek = peek(at: value.location.y) }
        }
        .onEnded { value in
          defer { dragAxis = nil }
          if dragAxis == .horizontal {
            if abs(value.translation.width) > 30 { step(value.translation.width < 0 ? 1 : -1) }
          } else if app.sidebarPeek != nil {
            finishPeek(predicted: peek(at: value.predictedEndLocation.y))
          }
        })
  }

  private var screenHeight: CGFloat {
    (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.screen.bounds.height ?? 800
  }

  /// The sidebar's 0...1 position with its top edge under the finger (at
  /// `y` on screen): it comes up with the finger, not behind it.
  private func peek(at y: CGFloat) -> CGFloat {
    let height = max(screenHeight - SidebarPeek.top(app), 1)
    return min(max((screenHeight - y) / height, 0), 1)
  }

  /// Open if it's past halfway, or flicked up far enough to get there;
  /// otherwise slide back down. Opening hands over to the real sheet, with
  /// no animation, where the peek ends.
  private func finishPeek(predicted: CGFloat) {
    let open = max(app.sidebarPeek ?? 0, predicted) > 0.5 && predicted > 0.3
    withAnimation(.snappy(duration: 0.3)) {
      app.sidebarPeek = open ? 1 : 0
    } completion: {
      guard open else {
        app.sidebarPeek = nil
        return
      }
      var instant = Transaction()
      instant.disablesAnimations = true
      withTransaction(instant) { app.showingSidebar = true }
      // The sheet is over the peek by then; drop it once the sheet is in.
      Task {
        try? await Task.sleep(for: .milliseconds(400))
        app.sidebarPeek = nil
      }
    }
  }

  /// The site's icon and host, centered; the load progress as a thin line in
  /// the space's color along its bottom edge.
  private var addressPill: some View {
    Button { app.showingCommandBar = true } label: {
      HStack(spacing: 7) {
        if let tab = app.shownTab {
          SiteIcon(url: tab.url, size: 16)
        } else {
          Image(systemName: "magnifyingglass").font(.system(size: 14, weight: .semibold)).opacity(0.5)
        }
        Text(host)
          .font(.system(size: 16, weight: app.shownTab == nil ? .regular : .medium))
          .foregroundStyle(app.shownTab == nil ? .secondary : .primary)
          .lineLimit(1)
      }
      .padding(.horizontal, 14)
      .frame(maxWidth: .infinity)
      .frame(height: 44)
      .background(Color.primary.opacity(0.07), in: Capsule())
      .overlay(alignment: .bottom) {
        if let progress = loadProgress {
          // Scaled, not measured: a GeometryReader here re-lays out the
          // glass bar endlessly.
          Capsule().fill(tint)
            .frame(maxWidth: .infinity)
            .frame(height: 2.5)
            .scaleEffect(x: progress, anchor: .leading)
            .animation(.easeOut(duration: 0.25), value: progress)
          .padding(.horizontal, 16)
          .transition(.opacity)
        }
      }
      .contentShape(Capsule())
    }
    .accessibilityLabel("Address")
    .accessibilityValue(host)
  }

  private var host: String {
    guard let tab = app.shownTab, let url = URL(string: tab.url) else { return "Search or enter address" }
    return url.host()?.replacingOccurrences(of: "www.", with: "") ?? tab.url
  }

  /// The next or previous entry of the space: pinned, then Today (newest first).
  private func step(_ delta: Int) {
    guard let node, let space = app.store.space(of: node) else { return }
    let entries = (space.pinned.children.flatMap(\.subtree) + space.today.children.reversed())
      .filter { !$0.isFolder && $0.parent?.isSplit != true && $0.isPhoneVisible }
    guard let index = entries.firstIndex(where: { $0 === node }) else { return }
    let next = index + delta
    guard entries.indices.contains(next) else { return }
    withAnimation(.easeInOut(duration: 0.2)) { app.show(entries[next]) }
  }
}

/// A bottom-bar glyph on a 44 pt round target.
private struct BarIcon: View {
  let symbol: String

  var body: some View {
    Image(systemName: symbol)
      .font(.system(size: 18, weight: .semibold))
      .frame(width: 44, height: 44)
      .contentShape(Circle())
  }
}
