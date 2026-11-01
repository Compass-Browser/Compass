// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI

/// Search or type an address; or jump to an open entry, a pinned tab or a
/// favorite. Everything it offers is the current space's profile's: its
/// spaces, history and other devices' tabs (M8), never another profile's.
struct CommandBar: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State private var text = ""
  @FocusState private var focused: Bool
  /// Navigate the shown tab instead of opening a new one.
  let replacing: SidebarNode?

  var body: some View {
    NavigationStack {
      List {
        if !text.isEmpty {
          Button {
            app.open(text, replacing: replacing)
            dismiss()
          } label: {
            Label(text, systemImage: looksLikeURL ? "globe" : "magnifyingglass")
          }
        }
        if !matches.isEmpty {
          Section("Switch to") {
            ForEach(matches) { node in
              Button {
                app.show(node)
                dismiss()
              } label: {
                HStack {
                  SiteIcon(url: node.url)
                  VStack(alignment: .leading) {
                    Text(node.displayTitle).lineLimit(1).foregroundStyle(.primary)
                    Text(spaceName(node)).font(.caption).foregroundStyle(.secondary)
                  }
                }
              }
            }
          }
        }
        let pages = app.sync.history.search(text, limit: 8, profile: account)
        if !pages.isEmpty {
          Section(text.isEmpty ? "Recently visited" : "History") {
            ForEach(pages) { page in
              Button {
                app.open(page.url, replacing: replacing)
                dismiss()
              } label: {
                HStack {
                  SiteIcon(url: page.url)
                  VStack(alignment: .leading) {
                    Text(page.title.isEmpty ? page.url : page.title).lineLimit(1).foregroundStyle(.primary)
                    Text(page.url).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                  }
                }
              }
            }
          }
        }
        ForEach(app.sync.foreignSessions(account: account)) { session in
          let tabs = session.tabs.filter {
            PhoneVisible.url($0.url)
          }.filter {
            text.isEmpty || $0.title.localizedCaseInsensitiveContains(text) || $0.url.localizedCaseInsensitiveContains(text)
          }
          if !tabs.isEmpty {
            Section("Tabs on \(session.name)") {
              ForEach(tabs.prefix(text.isEmpty ? 5 : 20)) { tab in
                Button {
                  app.open(tab.url)
                  dismiss()
                } label: {
                  HStack {
                    SiteIcon(url: tab.url)
                    Text(tab.title.isEmpty ? tab.url : tab.title).lineLimit(1).foregroundStyle(.primary)
                  }
                }
              }
            }
          }
        }
      }
      .safeAreaInset(edge: .top) {
        TextField("Search or enter address", text: $text)
          .textFieldStyle(.roundedBorder)
          .keyboardType(.webSearch)
          .textInputAutocapitalization(.never)
          .autocorrectionDisabled()
          .submitLabel(.go)
          .focused($focused)
          .onSubmit {
            guard !text.isEmpty else { return }
            app.open(text, replacing: replacing)
            dismiss()
          }
          .padding()
      }
      .navigationTitle(replacing == nil ? "New Tab" : "Go to")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar { Button("Cancel") { dismiss() } }
    }
    .onAppear {
      text = replacing?.url ?? ""
      focused = true
    }
  }

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
}
