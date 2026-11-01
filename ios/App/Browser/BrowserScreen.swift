// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI
import WebKit

/// One entry, full screen, with a slim bottom bar: back, the address pill
/// (opens the command bar), forward and the sidebar. Swiping the bar left
/// or right moves between the space's entries.
struct BrowserScreen: View {
  @Environment(AppModel.self) private var app
  let node: SidebarNode

  var body: some View {
    @Bindable var app = app
    VStack(spacing: 0) {
      if node.isSplit {
        Picker("Pane", selection: $app.selectedPane) {
          ForEach(Array(node.children.enumerated()), id: \.offset) { index, pane in
            Text(pane.displayTitle).lineLimit(1).tag(index)
          }
        }
        .pickerStyle(.segmented)
        .padding(.horizontal)
        .padding(.vertical, 6)
      }
      if let tab = app.shownTab {
        WebView(webView: app.tabs.webView(for: tab))
          .id(tab.id)
          .ignoresSafeArea(edges: node.isSplit ? [] : .top)
          .overlay(alignment: .top) { progress(tab) }
      } else {
        Spacer()
      }
      bar
    }
  }

  @ViewBuilder private func progress(_ tab: SidebarNode) -> some View {
    let _ = app.tabs.revision
    if let view = app.tabs.live[tab.id], view.isLoading {
      ProgressView(value: view.estimatedProgress)
        .progressViewStyle(.linear)
        .tint(app.store.space(of: tab)?.color.color ?? .accentColor)
    }
  }

  private var bar: some View {
    let _ = app.tabs.revision
    let view = app.shownTab.flatMap { app.tabs.live[$0.id] }
    return HStack(spacing: 18) {
      Button { view?.goBack() } label: { Image(systemName: "chevron.left") }
        .disabled(!(view?.canGoBack ?? false))
      Button { app.showingCommandBar = true } label: {
        HStack(spacing: 6) {
          if let tab = app.shownTab { SiteIcon(url: tab.url, size: 14) }
          Text(host)
            .font(.callout)
            .lineLimit(1)
            .foregroundStyle(.primary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 8)
        .background(.quaternary.opacity(0.6), in: Capsule())
      }
      Button { view?.goForward() } label: { Image(systemName: "chevron.right") }
        .disabled(!(view?.canGoForward ?? false))
      Button { app.showSidebar() } label: { Image(systemName: "sidebar.left") }
        .accessibilityLabel("Sidebar")
    }
    .font(.title3)
    .padding(.horizontal, 18)
    .padding(.vertical, 8)
    .background(.bar)
    .gesture(
      DragGesture(minimumDistance: 30).onEnded { value in
        guard abs(value.translation.width) > abs(value.translation.height) else { return }
        step(value.translation.width < 0 ? 1 : -1)
      })
  }

  private var host: String {
    guard let tab = app.shownTab, let url = URL(string: tab.url) else { return "Search or enter address" }
    return url.host()?.replacingOccurrences(of: "www.", with: "") ?? tab.url
  }

  /// The next or previous entry of the space: pinned, then Today (newest first).
  private func step(_ delta: Int) {
    guard let space = app.store.space(of: node) else { return }
    let entries = (space.pinned.children.flatMap(\.subtree) + space.today.children.reversed())
      .filter { !$0.isFolder && $0.parent?.isSplit != true }
    guard let index = entries.firstIndex(where: { $0 === node }) else { return }
    let next = index + delta
    guard entries.indices.contains(next) else { return }
    withAnimation(.easeInOut(duration: 0.2)) { app.show(entries[next]) }
  }
}
