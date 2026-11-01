// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI

/// Search or type an address; or jump to an open entry, a pinned tab or a
/// favorite in any space.
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

  private var matches: [SidebarNode] {
    let q = text.lowercased()
    let all = app.store.allNodes.filter { $0.isTab && $0.section != .archive }
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
