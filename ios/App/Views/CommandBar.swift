// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI

/// Search or type an address; or jump to an open entry, a pinned tab or a
/// favorite. Everything it offers is the current space's profile's: its
/// spaces, history and other devices' tabs (M8), never another profile's.
///
/// The look: the space's tint behind everything, as in the sidebar; a large
/// search field on top with Cancel beside it; suggestions in rounded cards.
struct CommandBar: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State private var text = ""
  @FocusState private var focused: Bool
  /// Navigate the shown tab instead of opening a new one.
  let replacing: SidebarNode?

  var body: some View {
    VStack(spacing: 0) {
      header
      ScrollView {
        LazyVStack(alignment: .leading, spacing: 4) {
          if !text.isEmpty {
            Card {
              Row(title: Self.short(text), subtitle: looksLikeURL ? "Open address" : "Search DuckDuckGo",
                  trailing: "return", action: { go(text) }) {
                Image(systemName: looksLikeURL ? "globe" : "magnifyingglass")
                  .font(.system(size: 15, weight: .semibold))
                  .foregroundStyle(.white)
                  .frame(width: 32, height: 32)
                  .background(tint, in: RoundedRectangle(cornerRadius: 9))
              }
            }
            .padding(.bottom, 8)
          }
          if !matches.isEmpty {
            CardSection(text.isEmpty ? "Today" : "Switch to tab") {
              ForEach(matches) { node in
                Row(title: node.displayTitle, subtitle: spaceName(node), trailing: "arrow.right",
                    action: { app.show(node); dismiss() }) { SiteTile(url: node.url) }
              }
            }
          }
          let pages = app.sync.history.search(text, limit: 8, profile: account)
          if !pages.isEmpty {
            CardSection(text.isEmpty ? "Recently visited" : "History") {
              ForEach(pages) { page in
                Row(title: page.title.isEmpty ? Self.short(page.url) : page.title,
                    subtitle: Self.short(page.url), action: { go(page.url) }) { SiteTile(url: page.url) }
              }
            }
          }
          ForEach(app.sync.foreignSessions(account: account)) { session in
            let tabs = session.tabs.filter {
              PhoneVisible.url($0.url)
            }.filter {
              text.isEmpty || $0.title.localizedCaseInsensitiveContains(text)
                || $0.url.localizedCaseInsensitiveContains(text)
            }
            if !tabs.isEmpty {
              CardSection("On \(session.name)") {
                ForEach(tabs.prefix(text.isEmpty ? 5 : 20)) { tab in
                  Row(title: tab.title.isEmpty ? Self.short(tab.url) : tab.title,
                      subtitle: Self.short(tab.url), action: { app.open(tab.url); dismiss() }) {
                    SiteTile(url: tab.url)
                  }
                }
              }
            }
          }
        }
        .padding(.horizontal, 14)
        .padding(.bottom, 24)
      }
      .scrollDismissesKeyboard(.interactively)
    }
    .background(SpaceTint(color: tint))
    .presentationDragIndicator(.visible)
    .onAppear {
      text = replacing?.url ?? ""
      focused = true
    }
  }

  /// "New tab in Personal" over the field and Cancel beside it.
  private var header: some View {
    VStack(alignment: .leading, spacing: 10) {
      HStack(spacing: 6) {
        if let space = app.selectedSpace {
          SpaceIcon(space: space, size: 13).foregroundStyle(tint)
        }
        Text(replacing == nil ? "New tab in \(app.selectedSpace?.name ?? "Compass")" : "Go to")
          .font(.footnote.weight(.semibold))
          .foregroundStyle(.secondary)
      }
      .padding(.leading, 4)
      HStack(spacing: 12) {
        HStack(spacing: 8) {
          Image(systemName: "magnifyingglass")
            .font(.system(size: 16, weight: .semibold))
            .foregroundStyle(.secondary)
          TextField("Search or enter address", text: $text)
            .font(.system(size: 17))
            .keyboardType(.webSearch)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .submitLabel(.go)
            .focused($focused)
            .onSubmit { if !text.isEmpty { go(text) } }
          if !text.isEmpty {
            Button { text = "" } label: {
              Image(systemName: "xmark.circle.fill")
                .font(.system(size: 16))
                .foregroundStyle(.tertiary)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Clear")
          }
        }
        .padding(.horizontal, 12)
        .frame(height: 46)
        .background(cardFill, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(
          RoundedRectangle(cornerRadius: 14, style: .continuous)
            .strokeBorder(tint.opacity(focused ? 0.55 : 0), lineWidth: 1.5)
        )
        Button("Cancel") { dismiss() }
          .font(.body.weight(.medium))
          .foregroundStyle(.primary)
          .buttonStyle(.plain)
      }
    }
    .padding(.horizontal, 14)
    .padding(.top, 22)
    .padding(.bottom, 10)
  }

  private func go(_ address: String) {
    app.open(address, replacing: replacing)
    dismiss()
  }

  private var tint: Color { app.selectedSpace?.color.color ?? .accentColor }

  private var looksLikeURL: Bool { AppModel.url(for: text).hasPrefix("https://duckduckgo.com") == false }

  /// The profile account of the current space.
  private var account: String { app.selectedSpace.map(app.sync.accountProfile(of:)) ?? "" }

  private var matches: [SidebarNode] {
    let q = text.lowercased()
    let all = app.store.tabs(inProfileOf: app.selectedSpace).filter(\.isPhoneVisible)
    let found = q.isEmpty ? all.filter { $0.section == .today } : all.filter {
      $0.displayTitle.lowercased().contains(q) || $0.url.lowercased().contains(q)
    }
    return Array(found.prefix(12))
  }

  private func spaceName(_ node: SidebarNode) -> String {
    if node.section == .favorites { return "Favorites" }
    return "\(app.store.space(of: node)?.name ?? "") · \(node.section == .pinned ? "Pinned" : "Today")"
  }

  /// An address without its scheme, "www." or trailing slash.
  static func short(_ url: String) -> String {
    var s = url
    for prefix in ["https://", "http://", "www."] where s.hasPrefix(prefix) { s.removeFirst(prefix.count) }
    if s.hasSuffix("/") { s.removeLast() }
    return s
  }
}

/// A light rounded card over the space's tint, its rows split by hairlines.
/// Each row draws a hairline on its top edge; the card hides the first one.
private struct Card<Content: View>: View {

  @ViewBuilder var content: Content

  var body: some View {
    VStack(spacing: 0) { content }
      .padding(.top, -1)
      .background(cardFill, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
  }
}

/// A small uppercase title over a card, as the sidebar's section headers.
private struct CardSection<Content: View>: View {
  let title: String
  @ViewBuilder var content: Content

  init(_ title: String, @ViewBuilder content: () -> Content) {
    self.title = title
    self.content = content()
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      Text(title.uppercased())
        .font(.system(size: 12, weight: .semibold))
        .tracking(0.4)
        .opacity(0.55)
        .lineLimit(1)
        .padding(.leading, 6)
      Card { content }
    }
    .padding(.top, 10)
  }
}

/// One suggestion: an icon tile, a title over a dimmed subtitle, and an
/// optional hint of what tapping does.
private struct Row<Icon: View>: View {
  let title: String
  let subtitle: String
  var trailing: String? = nil
  let action: () -> Void
  @ViewBuilder var icon: Icon

  var body: some View {
    Button(action: action) {
      HStack(spacing: 12) {
        icon
        VStack(alignment: .leading, spacing: 1) {
          Text(title).font(.system(size: 16, weight: .medium)).lineLimit(1)
          Text(subtitle).font(.footnote).foregroundStyle(.secondary).lineLimit(1)
        }
        Spacer(minLength: 4)
        if let trailing {
          Image(systemName: trailing)
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(.tertiary)
        }
      }
      .padding(.horizontal, 12)
      .frame(minHeight: 56)
      .contentShape(Rectangle())
    }
    .overlay(alignment: .top) {
      Rectangle().fill(.separator).frame(height: 1 / 3).padding(.leading, 56)
    }
    .buttonStyle(RowPressStyle())
  }
}

/// A site's favicon on a soft tile, so icons of any shape line up.
private struct SiteTile: View {
  let url: String

  var body: some View {
    SiteIcon(url: url, size: 18)
      .frame(width: 32, height: 32)
      .background(Color.primary.opacity(0.06), in: RoundedRectangle(cornerRadius: 9))
  }
}

/// Rows dim slightly while pressed, without the system tint.
private struct RowPressStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .foregroundStyle(.primary)
      .background(Color.primary.opacity(configuration.isPressed ? 0.08 : 0))
  }
}

/// The cards' and the search field's fill over the space's tint.
private let cardFill = Color(.secondarySystemGroupedBackground).opacity(0.82)
