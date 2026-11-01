// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI
import WebKit

/// One entry, full screen, with a slim bottom bar: back, the address pill
/// (opens the command bar), forward and the sidebar. Swiping the bar left
/// or right moves between the space's entries; swiping it up opens the
/// sidebar. With no entry selected it's an empty page in the space's color.
struct BrowserScreen: View {
  @Environment(AppModel.self) private var app
  let node: SidebarNode?

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
      if let tab = app.shownTab {
        WebView(webView: app.tabs.webView(for: tab))
          .id(tab.id)
          .ignoresSafeArea(edges: node?.isSplit == true ? [] : .top)
          .overlay(alignment: .top) { progress(tab) }
      } else {
        emptyPage
      }
      if let pending = app.passwords.pendingSave { saveBanner(pending) }
      bar
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
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14))
    .padding(.horizontal, 10)
    .padding(.bottom, 6)
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
          Image(systemName: "key.fill")
        }
        .accessibilityLabel("Fill password")
      }
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
        .opacity(view?.canGoBack == true ? 1 : 0.3)
      Button { app.showingCommandBar = true } label: {
        HStack(spacing: 6) {
          if let tab = app.shownTab { SiteIcon(url: tab.url, size: 14) }
          Text(host)
            .font(.callout)
            .lineLimit(1)
            .foregroundStyle(.primary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 9)
        .background(SidebarStyle.fill, in: Capsule())
      }
      Button { view?.goForward() } label: { Image(systemName: "chevron.right") }
        .disabled(!(view?.canGoForward ?? false))
        .opacity(view?.canGoForward == true ? 1 : 0.3)
      keyButton
      Button { app.showSidebar() } label: { Image(systemName: "sidebar.left") }
        .accessibilityLabel("Sidebar")
    }
    .font(.system(size: 19, weight: .medium))
    // The sidebar's neutral look, not the system tint.
    .buttonStyle(.plain)
    .foregroundStyle(.primary)
    .padding(.horizontal, 18)
    .padding(.vertical, 8)
    .background(.bar)
    .gesture(
      DragGesture(minimumDistance: 30).onEnded { value in
        let (dx, dy) = (value.translation.width, value.translation.height)
        if abs(dx) > abs(dy) {
          step(dx < 0 ? 1 : -1)
        } else if dy < 0 {
          app.showSidebar()
        }
      })
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
