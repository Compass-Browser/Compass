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
  @State private var showingDownloads = false
  @State private var showingArchive = false

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
        // Not in incognito (as the desktop).
        if !(tab.map { app.tabs.isIncognito($0.id) } ?? false) {
        Tile(title: "Ask Claude", symbol: "sparkles") {
          dismiss()
          // After the menu has gone: a view shows one sheet at a time.
          Task {
            try? await Task.sleep(for: .milliseconds(450))
            app.showingClaude = true
          }
        }
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
      HStack(spacing: 10) {
        WideButton(title: "Downloads", symbol: "arrow.down.circle", badge: downloadsBadge) { showingDownloads = true }
        WideButton(title: "Archive", symbol: "archivebox") { showingArchive = true }
      }
      if #available(iOS 18.4, *) {
        ExtensionBar(tab: tab?.id, page: view?.url ?? tab.flatMap { URL(string: $0.url) })
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
        .accessibilityIdentifier("MenuSettings")
      }
    }
    .padding(.horizontal, 14)
    .padding(.top, 26)
    .frame(maxHeight: .infinity, alignment: .top)
    .background(SpaceTint(theme: app.selectedSpace?.theme ?? .blue))
    .spaceAppearance(app.selectedSpace?.theme)
    .presentationDetents([.height(404 + extensionsHeight(tab, view))])
    .presentationDragIndicator(.visible)
    .sheet(isPresented: Binding(get: { app.showingSiteSettings }, set: { app.showingSiteSettings = $0 })) {
      SiteSettingsView(site: site)
    }
    .sheet(isPresented: $showingDownloads) {
      NavigationStack {
        DownloadsView().toolbar { Button("Done") { showingDownloads = false } }
      }
    }
    .sheet(isPresented: $showingArchive) { ArchiveView(space: nil) }
  }

  private func extensionsHeight(_ tab: SidebarNode?, _ view: WKWebView?) -> CGFloat {
    guard #available(iOS 18.4, *) else { return 0 }
    return ExtensionBar.height(app, tab: tab?.id, page: view?.url ?? tab.flatMap { URL(string: $0.url) })
  }

  /// Running downloads, as a count on the Downloads button.
  private var downloadsBadge: Int { app.downloads.items.filter(\.running).count }

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
      guard let tab, let space, !space.isIncognito else { return }
      withAnimation(SidebarStyle.motion) {
        if pinned {
          _ = app.store.move(tab, to: space.today, at: space.today.children.count)
        } else {
          _ = app.store.move(tab, to: space.pinned, at: space.pinned.children.count)
        }
      }
    }
    .disabled(tab == nil || space == nil || !(tab?.section == .today || tab?.section == .pinned)
      || tab?.parent?.isSplit == true || space?.isIncognito == true)
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

/// A half-width capsule button with a symbol and a title (Downloads,
/// Archive), and an optional count.
private struct WideButton: View {
  let title: String
  let symbol: String
  var badge = 0
  let action: () -> Void

  var body: some View {
    Button(action: action) {
      HStack(spacing: 8) {
        Image(systemName: symbol).font(.system(size: 17, weight: .medium))
        Text(title).font(.body.weight(.medium))
        if badge > 0 {
          Text("\(badge)")
            .font(.caption.weight(.bold))
            .foregroundStyle(.white)
            .padding(.horizontal, 6)
            .frame(minWidth: 20, minHeight: 20)
            .background(Color.accentColor, in: Capsule())
        }
      }
      .foregroundStyle(.primary)
      .frame(maxWidth: .infinity)
      .frame(height: 50)
      .background(cardFill, in: Capsule())
    }
    .buttonStyle(TileStyle())
    .accessibilityLabel(title)
  }
}

@available(iOS 18.4, *)
extension ExtensionBar {
  /// The long-press menu of an extension's button: its own items, then
  /// "Allow on <site>" (when it runs only on chosen sites), Options and
  /// Turn Off.
  func menu(_ record: WebExtensions.Record, _ action: WKWebExtension.Action) -> UIMenu {
    let extensions = app.extensions
    var ours: [UIMenuElement] = []
    if let site = page?.host().map({ $0.hasPrefix("www.") ? String($0.dropFirst(4)) : $0 }),
      page?.scheme?.hasPrefix("http") == true, record.onlySites != nil
    {
      let allowed = record.onlySites?.contains(site) == true
      ours.append(UIAction(title: "Allow on \(site)", image: UIImage(systemName: "globe"), state: allowed ? .on : .off) { _ in
        extensions.setAllowed(record.id, site: site, !allowed)
      })
    }
    if extensions.context(record.id)?.optionsPageURL != nil {
      ours.append(UIAction(title: "Options", image: UIImage(systemName: "gearshape")) { _ in
        dismiss()
        Task {
          try? await Task.sleep(for: .milliseconds(450))
          extensions.openOptions(record.id)
        }
      })
    }
    ours.append(UIAction(title: "Turn Off", image: UIImage(systemName: "power"), attributes: .destructive) { _ in
      extensions.setEnabled(record.id, false)
    })
    let own = action.menuItems
    return UIMenu(title: action.label, children: own.isEmpty ? ours : [UIMenu(options: .displayInline, children: own)] + ours)
  }
}

/// A clear UIKit button over a SwiftUI label: a tap runs `action`, a long
/// press shows `menu` (extensions' own items are UIKit menu elements).
struct MenuButton: UIViewRepresentable {
  let label: String
  let menu: () -> UIMenu
  let action: () -> Void

  func makeUIView(context: Context) -> UIButton {
    let button = UIButton(type: .custom)
    button.showsMenuAsPrimaryAction = false
    update(button)
    return button
  }

  func updateUIView(_ button: UIButton, context: Context) { update(button) }

  private func update(_ button: UIButton) {
    button.accessibilityLabel = label
    // The same identifier replaces the previous update's action.
    button.addAction(UIAction(identifier: .init("tap")) { _ in action() }, for: .primaryActionTriggered)
    button.menu = menu()
  }
}

/// The page menu's extensions: "Add to Compass" on a store's extension
/// page, and a button for each running extension's action (its icon and
/// badge). A tap closes the menu and runs it: its popup comes up as a
/// sheet, or its click goes to the page.
@available(iOS 18.4, *)
struct ExtensionBar: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  let tab: String?
  let page: URL?

  /// The height it adds to the menu.
  static func height(_ app: AppModel, tab: String?, page: URL?) -> CGFloat {
    (storeSource(page) != nil ? 64 : 0) + (app.extensions.actions(forTab: tab).isEmpty ? 0 : 64)
  }

  static func storeSource(_ page: URL?) -> ExtensionSource? {
    guard let page, let source = ExtensionSource(page.absoluteString) else { return nil }
    return source
  }

  var body: some View {
    let actions = app.extensions.actions(forTab: tab)
    if let source = Self.storeSource(page) {
      Button {
        Task { await app.extensions.fetch(source) }
      } label: {
        HStack(spacing: 8) {
          if app.extensions.fetching {
            ProgressView()
          } else {
            Image(systemName: "puzzlepiece.extension.fill").font(.system(size: 17, weight: .medium))
          }
          Text(app.extensions.records.contains { $0.source == source } ? "Update in Compass" : "Add to Compass")
            .font(.body.weight(.semibold))
        }
        .foregroundStyle(.white)
        .frame(maxWidth: .infinity)
        .frame(height: 50)
        .background(Color.accentColor, in: Capsule())
      }
      .buttonStyle(.plain)
      .disabled(app.extensions.fetching)
      .accessibilityIdentifier("AddToCompass")
    }
    if !actions.isEmpty {
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 10) {
          ForEach(actions, id: \.record.id) { record, action in
            ExtensionIcon(image: action.icon(for: CGSize(width: 28, height: 28)), size: 28)
              .opacity(action.isEnabled ? 1 : 0.4)
              .frame(width: 50, height: 50)
              .background(cardFill, in: Circle())
              .overlay(alignment: .topTrailing) {
                if !action.badgeText.isEmpty {
                  Text(action.badgeText)
                    .font(.system(size: 10, weight: .bold))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 4)
                    .frame(minWidth: 18, minHeight: 18)
                    .background(Color.red, in: Capsule())
                }
              }
              .allowsHitTesting(false)
              .overlay {
                MenuButton(label: action.label, menu: { menu(record, action) }) {
                  dismiss()
                  // Once the menu is gone, so its popup has somewhere to go.
                  Task {
                    try? await Task.sleep(for: .milliseconds(action.presentsPopup ? 450 : 0))
                    app.extensions.performAction(record.id, tab: tab)
                  }
                }
              }
          }
        }
      }
      .frame(height: 54)
    }
  }
}
