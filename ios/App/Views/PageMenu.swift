// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI
import WebKit

/// The bottom bar's menu: what to do with the shown page. On top, back,
/// forward, the address (tap to edit), copy link and reload; then Find on
/// Page, Desktop Site, Pin and Share; then Site Settings and Settings.
struct PageMenu: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State private var copied = false

  var body: some View {
    let _ = app.tabs.revision
    let tab = app.shownTab
    let view = tab.flatMap { app.tabs.live[$0.id] }
    let site = SiteSettings.site(view?.url ?? tab.flatMap { URL(string: $0.url) })
    VStack(spacing: 14) {
      addressCard(view)
      HStack(spacing: 10) {
        Tile(title: "Find", symbol: "doc.text.magnifyingglass") {
          dismiss()
          app.findOnPage()
        }
        let desktop = app.siteSettings.value(.desktopSite, site: site)
        Tile(title: desktop ? "Mobile Site" : "Desktop Site", symbol: desktop ? "iphone" : "desktopcomputer",
             on: desktop) {
          guard let site else { return }
          // Reloads the site's tabs (TabManager.apply).
          app.siteSettings.set(.desktopSite, !desktop, site: site)
        }
        .disabled(site == nil)
        pinTile(tab)
        if let url = view?.url ?? tab.flatMap({ URL(string: $0.url) }) {
          ShareLink(item: url, subject: Text(tab?.displayTitle ?? "")) {
            TileLabel(title: "Share", symbol: "square.and.arrow.up")
          }
          .buttonStyle(TileStyle())
        }
      }
      Card {
        Row(title: "Site Settings", subtitle: site ?? "No page", trailing: "chevron.right",
            action: { app.showingSiteSettings = true }) {
          Image(systemName: "slider.horizontal.3").menuIcon(.blue)
        }
        Row(title: "Settings", subtitle: "Sync, passwords, search engine", trailing: "chevron.right",
            action: {
              dismiss()
              // Once the menu is gone: one sheet at a time.
              Task {
                try? await Task.sleep(for: .milliseconds(450))
                app.showingSettings = true
              }
            }) {
          Image(systemName: "gearshape.fill").menuIcon(.gray)
        }
      }
    }
    .padding(.horizontal, 14)
    .padding(.top, 26)
    .frame(maxHeight: .infinity, alignment: .top)
    .background(SpaceTint(color: app.selectedSpace?.color.color ?? .blue))
    .presentationDetents([.height(352)])
    .presentationDragIndicator(.visible)
    .sheet(isPresented: Binding(get: { app.showingSiteSettings }, set: { app.showingSiteSettings = $0 })) {
      SiteSettingsView(site: site)
    }
  }

  /// Back, forward, the host (tap: edit the address), copy link, reload.
  private func addressCard(_ view: WKWebView?) -> some View {
    HStack(spacing: 0) {
      MenuIcon(symbol: "chevron.backward", enabled: view?.canGoBack == true) { view?.goBack() }
      MenuIcon(symbol: "chevron.forward", enabled: view?.canGoForward == true) { view?.goForward() }
      Button {
        dismiss()
        app.openCommandBar(newTab: app.shownTab == nil)
      } label: {
        Text(host)
          .font(.system(size: 16, weight: .medium))
          .lineLimit(1)
          .frame(maxWidth: .infinity)
          .frame(height: 44)
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      MenuIcon(symbol: copied ? "checkmark" : "link", enabled: view?.url != nil) {
        UIPasteboard.general.url = view?.url
        copied = true
        Task {
          try? await Task.sleep(for: .seconds(1.2))
          copied = false
        }
      }
      .accessibilityLabel("Copy Link")
      .accessibilityValue(copied ? "Copied" : "")
      MenuIcon(symbol: "arrow.clockwise", enabled: view != nil) { view?.reload() }
        .accessibilityLabel("Reload")
    }
    .padding(.horizontal, 6)
    .frame(height: 56)
    .background(cardFill, in: Capsule())
  }

  @ViewBuilder private func pinTile(_ tab: SidebarNode?) -> some View {
    let space = tab.flatMap { app.store.space(of: $0) }
    let pinned = tab?.section == .pinned
    Tile(title: pinned ? "Unpin" : "Pin", symbol: pinned ? "pin.slash" : "pin", on: pinned) {
      guard let tab, let space else { return }
      withAnimation(SidebarStyle.motion) {
        if pinned {
          _ = app.store.move(tab, to: space.today, at: space.today.children.count)
        } else {
          _ = app.store.move(tab, to: space.pinned, at: space.pinned.children.count)
        }
      }
    }
    .disabled(tab == nil || space == nil || !(tab?.section == .today || tab?.section == .pinned)
      || tab?.parent?.isSplit == true)
  }

  private var host: String {
    guard let tab = app.shownTab, let url = URL(string: tab.url) else { return "No page" }
    return url.host()?.replacingOccurrences(of: "www.", with: "") ?? tab.url
  }
}

/// A square action tile: a symbol over a short title; tinted while `on`.
private struct Tile: View {
  let title: String
  let symbol: String
  var on = false
  let action: () -> Void

  var body: some View {
    Button(action: action) { TileLabel(title: title, symbol: symbol, on: on) }
      .buttonStyle(TileStyle())
      .accessibilityLabel(title)
  }
}

private struct TileLabel: View {
  @Environment(\.isEnabled) private var enabled
  let title: String
  let symbol: String
  var on = false

  var body: some View {
    VStack(spacing: 8) {
      Image(systemName: symbol).font(.system(size: 22, weight: .medium))
      Text(title).font(.caption.weight(.medium)).lineLimit(1).minimumScaleFactor(0.8)
    }
    .foregroundStyle(on ? Color.accentColor : .primary)
    .opacity(enabled ? 1 : 0.35)
    .frame(maxWidth: .infinity)
    .frame(height: 84)
    .background(cardFill, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
  }
}

private struct TileStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .scaleEffect(configuration.isPressed ? 0.96 : 1)
      .animation(.snappy(duration: 0.15), value: configuration.isPressed)
  }
}

/// A glyph button in the address card.
private struct MenuIcon: View {
  let symbol: String
  var enabled = true
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      Image(systemName: symbol)
        .font(.system(size: 18, weight: .semibold))
        .frame(width: 44, height: 44)
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .disabled(!enabled)
    .opacity(enabled ? 1 : 0.3)
  }
}

extension Image {
  /// A white glyph on a colored rounded square, as Settings' row icons.
  func menuIcon(_ color: Color) -> some View {
    self.font(.system(size: 15, weight: .semibold))
      .foregroundStyle(.white)
      .frame(width: 32, height: 32)
      .background(color, in: RoundedRectangle(cornerRadius: 9))
  }
}
