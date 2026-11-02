// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI
import WebKit

/// One entry, full screen, under a floating glass bar: back, the address
/// pill (opens the command bar; shows the load progress along its bottom),
/// forward when there's somewhere to go, and the sidebar. Swiping the bar
/// left or right moves between the space's entries, as in Safari: the page
/// and the bar follow the finger and the next entry's (its last snapshot
/// and its bar) slides in beside them; let go past a third of the screen,
/// or flick, and it takes over, else both spring back. Swiping it up opens,
/// by where the finger starts: on its left third the tab picker, in the
/// middle the sidebar (both follow the finger), on its right third the
/// command bar for a new tab. With no entry selected it's an empty page in
/// the space's color.
struct BrowserScreen: View {
  @Environment(AppModel.self) private var app
  let node: SidebarNode?
  /// The bar's drag: nil until it picks an axis.
  @State private var dragAxis: Axis?
  /// Where a swipe up on the bar started: what it opens.
  @State private var dragZone: Zone = .center
  /// A sideways swipe on the bar: how far the page and bar have moved.
  @State private var swipeX: CGFloat = 0
  /// The neighbors are drawn: from the swipe's start until it settles.
  @State private var swiping = false
  enum Zone { case left, center, right }
  /// The menu button's center on screen: where a download's arrow lands.
  @State private var menuCenter: CGPoint?
  /// A download's arrow on its way to the menu button (DownloadFlight).
  @State private var flight: DownloadFlight.Phase?
  @State private var menuBounce = false
  /// The bar's height over the page: the 44 pt pill, its 5 pt padding and
  /// the 2 pt gap under it. A constant, not measured: feeding a measured
  /// height back into the web view's inset loops layout.
  static let barHeight: CGFloat = 44 + 2 * 5 + 2
  /// Collapsed while the page scrolls down: a small address pill.
  static let collapsedHeight: CGFloat = 30 + 2

  var body: some View {
    @Bindable var app = app
    content
      .overlay {
        if let flight, let menuCenter {
          DownloadFlight(phase: flight, target: menuCenter)
        }
      }
      .onChange(of: app.downloads.started) { downloadStarted() }
  }

  /// A download started: an arrow pops up mid-screen and drops into the
  /// menu button, which bounces; a light tap goes with it.
  private func downloadStarted() {
    UIImpactFeedbackGenerator(style: .medium).impactOccurred()
    flight = .appear
    withAnimation(.spring(response: 0.3, dampingFraction: 0.6)) { flight = .shown }
    Task {
      try? await Task.sleep(for: .milliseconds(380))
      withAnimation(.easeIn(duration: 0.42)) { flight = .landed }
      try? await Task.sleep(for: .milliseconds(420))
      flight = nil
      withAnimation(.spring(response: 0.25, dampingFraction: 0.4)) { menuBounce = true }
      try? await Task.sleep(for: .milliseconds(180))
      withAnimation(.spring(response: 0.3, dampingFraction: 0.6)) { menuBounce = false }
    }
  }

  @ViewBuilder private var content: some View {
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
      page
      .overlay {
        if swiping {
          ZStack {
            if let next = neighbor(1) { preview(next).offset(x: Self.width) }
            if let previous = neighbor(-1) { preview(previous).offset(x: -Self.width) }
          }
          .allowsHitTesting(false)
        }
      }
      .offset(x: swipeX)
      .safeAreaInset(edge: .bottom, spacing: 0) {
        VStack(spacing: 8) {
          if let pending = app.passwords.pendingSave { saveBanner(pending) }
          if app.barCollapsed {
            collapsedBar
              .transition(.scale(scale: 0.6, anchor: .bottom).combined(with: .opacity))
          } else {
            bar
              .overlay {
                if swiping {
                  ZStack {
                    if let next = neighbor(1) { ghostBar(next).offset(x: Self.width) }
                    if let previous = neighbor(-1) { ghostBar(previous).offset(x: -Self.width) }
                  }
                  .allowsHitTesting(false)
                }
              }
              .offset(x: swipeX)
              .transition(.scale(scale: 0.9, anchor: .bottom).combined(with: .opacity))
          }
        }
        .animation(.snappy(duration: 0.25), value: app.passwords.pendingSave == nil)
      }
    }
  }

  private static var width: CGFloat { SidebarPanel.screenWidth }

  /// The shown entry's page, or its space's color when there's none.
  @ViewBuilder private var page: some View {
    Group {
      if let tab = app.shownTab {
        WebView(
          webView: app.tabs.webView(for: tab),
          bottomInset: app.barCollapsed ? Self.collapsedHeight : Self.barHeight)
          .id(tab.id)
          // The page runs under the bar; WKWebView insets its content by
          // the safe area, so the page's end still scrolls clear of it.
          .ignoresSafeArea(edges: node?.isSplit == true ? .bottom : [.top, .bottom])
      } else {
        emptyPage
      }
    }
  }

  /// The tab an entry shows (a split's first pane).
  private func tab(of node: SidebarNode) -> SidebarNode? {
    node.isSplit ? node.children.first(where: \.isPhoneVisible) ?? node.children.first : node
  }

  /// A neighbor entry's page while the bar is swiped: how it looked when it
  /// was left, or its icon and title when it has no snapshot (not loaded).
  private func preview(_ node: SidebarNode) -> some View {
    let tab = tab(of: node)
    return ZStack {
      Color(.systemBackground)
      if let tab, let image = app.tabs.snapshots[tab.id] {
        Image(uiImage: image)
          .resizable()
          .scaledToFill()
          .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
          .clipped()
      } else if let tab {
        VStack(spacing: 12) {
          SiteIcon(url: tab.url, size: 48)
          Text(tab.displayTitle).font(.headline).foregroundStyle(.secondary).lineLimit(2)
        }
        .padding(32)
      }
    }
    .ignoresSafeArea()
  }

  /// A neighbor entry's bar while the bar is swiped: the same capsule, its
  /// site in the pill.
  private func ghostBar(_ node: SidebarNode) -> some View {
    let tab = tab(of: node)
    return HStack(spacing: 2) {
      BarIcon(symbol: "chevron.backward").opacity(0.3)
      BarIcon(symbol: "chevron.forward").opacity(0.3)
      HStack(spacing: 7) {
        if let tab { SiteIcon(url: tab.url, size: 16) }
        Text(tab.map { host(of: $0.url) } ?? "").font(.system(size: 16, weight: .medium)).lineLimit(1)
      }
      .padding(.horizontal, 14)
      .frame(maxWidth: .infinity)
      .frame(height: 44)
      .background(Color.primary.opacity(0.07), in: Capsule())
      BarIcon(symbol: "plus")
      BarIcon(symbol: "ellipsis")
    }
    .foregroundStyle(.primary)
    .padding(5)
    .floatingGlass(in: Capsule())
    .padding(.horizontal, 12)
    .padding(.bottom, 2)
    .accessibilityHidden(true)
  }

  private var emptyPage: some View {
    SpaceTint(theme: app.selectedSpace?.theme ?? .blue)
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
    app.shownTab.flatMap { app.store.space(of: $0)?.theme.accent }
      ?? app.selectedSpace?.theme.accent ?? .accentColor
  }

  private var bar: some View {
    let _ = app.tabs.revision
    let view = app.shownTab.flatMap { app.tabs.live[$0.id] }
    let canBack = view?.canGoBack ?? false
    let canForward = view?.canGoForward ?? false
    return HStack(spacing: 2) {
      // Back and forward together on the left; both always there (dimmed
      // when there's nowhere to go), so nothing shifts.
      Button { view?.goBack() } label: { BarIcon(symbol: "chevron.backward") }
        .disabled(!canBack)
        .opacity(canBack ? 1 : 0.3)
        .accessibilityLabel("Back")
      Button { view?.goForward() } label: { BarIcon(symbol: "chevron.forward") }
        .disabled(!canForward)
        .opacity(canForward ? 1 : 0.3)
        .accessibilityLabel("Forward")
      addressPill
      keyButton
      Button { app.openCommandBar(newTab: true) } label: { BarIcon(symbol: "plus") }
        .accessibilityLabel("New tab")
        .accessibilityIdentifier("BarNewTab")
      Button { app.showingMenu = true } label: {
        BarIcon(symbol: "ellipsis")
          .scaleEffect(menuBounce ? 1.35 : 1)
          .onGeometryChange(for: CGPoint.self, of: { geo in
            let frame = geo.frame(in: .global)
            return CGPoint(x: frame.midX, y: frame.midY)
          }) { menuCenter = $0 }
          .overlay {
            // A download running: its progress around the menu button.
            if let progress = app.downloads.overallProgress {
              Circle().stroke(Color.primary.opacity(0.12), lineWidth: 2.5).frame(width: 30, height: 30)
              Circle().trim(from: 0, to: max(progress, 0.03))
                .stroke(tint, style: StrokeStyle(lineWidth: 2.5, lineCap: .round))
                .rotationEffect(.degrees(-90))
                .frame(width: 30, height: 30)
                .animation(.easeOut(duration: 0.2), value: progress)
            }
          }
      }
        .accessibilityLabel("Menu")
    }
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
          if dragAxis == nil {
            dragAxis = abs(dx) > abs(dy) ? .horizontal : .vertical
            let third = SidebarPanel.screenWidth / 3
            let x = value.startLocation.x
            dragZone = x < third ? .left : x > 2 * third ? .right : .center
            if dragAxis == .vertical, dragZone == .left { app.preparePicker() }
          }
          if dragAxis == .horizontal {
            swiping = true
            // Past the space's ends it only gives a little.
            swipeX = neighbor(dx < 0 ? 1 : -1) == nil ? dx / 4 : dx
            return
          }
          guard dragAxis == .vertical else { return }
          switch dragZone {
          case .left: app.pickerDrag = min(max(-dy / TabPicker.travel, 0), 1)
          case .center: app.sidebarDrag = sidebarPosition(at: value.location.y)
          case .right: break
          }
        }
        .onEnded { value in
          defer { dragAxis = nil }
          if dragAxis == .horizontal {
            settleSwipe(value.translation.width, predicted: value.predictedEndTranslation.width)
            return
          }
          switch dragZone {
          case .left:
            if app.pickerDrag != nil {
              app.settlePicker(predicted: -value.predictedEndTranslation.height / TabPicker.travel)
            }
          case .center:
            if let drag = app.sidebarDrag {
              app.settleSidebar(at: drag, predicted: sidebarPosition(at: value.predictedEndLocation.y))
            }
          case .right:
            if value.translation.height < -30 || value.predictedEndTranslation.height < -80 {
              app.openCommandBar(newTab: true)
            }
          }
        })
  }

  /// The sidebar's 0...1 position with its top edge under the finger (at
  /// `y` on screen): it comes up with the finger, not behind it.
  private func sidebarPosition(at y: CGFloat) -> CGFloat {
    min(max((SidebarPanel.screenHeight - y) / SidebarPanel.height, 0), 1)
  }

  /// The bar while the page scrolls down: the site's icon and host, small,
  /// on a narrow pill. A tap brings the whole bar back.
  private var collapsedBar: some View {
    Button { app.setBarCollapsed(false) } label: {
      HStack(spacing: 5) {
        if let tab = app.shownTab { SiteIcon(url: tab.url, size: 12) }
        Text(host).font(.system(size: 13, weight: .medium)).lineLimit(1)
      }
      .padding(.horizontal, 14)
      .frame(height: 30)
      .floatingGlass(in: Capsule())
      .contentShape(Capsule())
    }
    .buttonStyle(.plain)
    .foregroundStyle(.primary)
    .padding(.bottom, 2)
    .accessibilityLabel("Show toolbar")
    .accessibilityValue(host)
  }

  /// The site's icon and host, centered; the load progress as a thin line in
  /// the space's color along its bottom edge.
  private var addressPill: some View {
    // Edits the shown tab's address (a new tab when there's none).
    Button { app.openCommandBar(newTab: app.shownTab == nil) } label: {
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
    guard let tab = app.shownTab else { return "Search or enter address" }
    return host(of: tab.url)
  }

  private func host(of url: String) -> String {
    URL(string: url)?.host()?.replacingOccurrences(of: "www.", with: "") ?? url
  }

  /// The next (1) or previous (-1) entry of the space: pinned, then Today
  /// (newest first).
  private func neighbor(_ delta: Int) -> SidebarNode? {
    guard let node, let space = app.store.space(of: node) else { return nil }
    let entries = (space.pinned.children.flatMap(\.subtree) + space.today.children.reversed())
      .filter { !$0.isFolder && $0.parent?.isSplit != true && $0.isPhoneVisible }
    guard let index = entries.firstIndex(where: { $0 === node }) else { return nil }
    let next = index + delta
    return entries.indices.contains(next) ? entries[next] : nil
  }

  /// The swipe let go `dx` from where it started: past a third of the
  /// screen (or flicked past half) the neighbor slides in and is shown;
  /// otherwise the page springs back.
  private func settleSwipe(_ dx: CGFloat, predicted: CGFloat) {
    let delta = dx < 0 ? 1 : -1
    let flicked = abs(predicted) > Self.width / 2 && (predicted < 0) == (dx < 0)
    let spring = Animation.spring(duration: 0.32, bounce: 0)
    guard let next = neighbor(delta), abs(dx) > Self.width / 3 || flicked else {
      withAnimation(spring) { swipeX = 0 } completion: { swiping = false }
      return
    }
    withAnimation(spring) { swipeX = -CGFloat(delta) * Self.width } completion: {
      // The neighbor is where the page was: swap them in one frame.
      var instant = Transaction()
      instant.disablesAnimations = true
      withTransaction(instant) {
        app.show(next)
        swipeX = 0
        swiping = false
      }
    }
  }
}

/// A bottom-bar glyph on a 44 pt tall, 40 pt wide target (five fit beside
/// the address pill).
private struct BarIcon: View {
  let symbol: String

  var body: some View {
    Image(systemName: symbol)
      .font(.system(size: 18, weight: .semibold))
      .frame(width: 40, height: 44)
      .contentShape(Circle())
  }
}

/// A download's arrow: pops up in the middle of the screen, then drops into
/// the menu button (`target`, in screen points) and shrinks away.
struct DownloadFlight: View {
  enum Phase { case appear, shown, landed }
  let phase: Phase
  let target: CGPoint

  var body: some View {
    GeometryReader { geo in
      let frame = geo.frame(in: .global)
      let start = CGPoint(x: frame.midX, y: frame.midY - 40)
      let at = phase == .landed ? target : start
      Image(systemName: "arrow.down.circle.fill")
        .font(.system(size: 64, weight: .semibold))
        .symbolRenderingMode(.palette)
        .foregroundStyle(.white, Color.accentColor)
        .shadow(color: .black.opacity(0.25), radius: 12, y: 6)
        .scaleEffect(phase == .appear ? 0.3 : phase == .landed ? 0.28 : 1)
        .opacity(phase == .appear ? 0 : 1)
        .position(x: at.x - frame.minX, y: at.y - frame.minY)
    }
    .ignoresSafeArea()
    .allowsHitTesting(false)
    .accessibilityHidden(true)
  }
}
