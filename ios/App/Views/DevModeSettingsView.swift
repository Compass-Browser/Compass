// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI

/// Settings → Development Mode (docs/design/dev-mode.md, "iOS"): the sites,
/// with add and swipe to delete, as the desktop's compass://settings/devMode.
struct DevModeSettingsView: View {
  @State private var typed = ""
  @State private var note: String?

  var body: some View {
    let devMode = DevMode.shared
    Form {
      Section {
        ForEach(devMode.patterns, id: \.self) { pattern in
          Text(pattern).font(.body.monospaced())
        }
        .onDelete { offsets in
          for pattern in offsets.map({ devMode.patterns[$0] }) { devMode.remove(pattern) }
        }
        if devMode.patterns.isEmpty {
          Text("No sites yet.").foregroundStyle(.secondary)
        }
      } footer: {
        Text("""
          On these sites, reload always gets a fresh copy instead of using the cache, and a bar \
          above the page shows the full address with a Copy button. A site covers every port, so \
          localhost covers localhost:3000; *.example.com also covers its subdomains. You can also \
          turn it on or off from the page menu, or with ⌘⇧D on a keyboard.
          """)
      }
      Section {
        HStack {
          TextField("localhost", text: $typed)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .keyboardType(.URL)
            .onSubmit(add)
          Button("Add", action: add)
            .disabled(typed.trimmingCharacters(in: .whitespaces).isEmpty)
        }
      } header: {
        Text("Add a Site")
      } footer: {
        if let note { Text(note).foregroundStyle(.red) }
      }
    }
    .navigationTitle("Development Mode")
    .navigationBarTitleDisplayMode(.inline)
  }

  private func add() {
    let input = typed.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !input.isEmpty else { return }
    if DevMode.shared.add(input) == nil {
      note = String(
        localized: "“\(input)” isn't a site, like localhost or example.com.",
        comment: "Development Mode settings: the typed text isn't a site.")
    } else {
      note = nil
      typed = ""
    }
  }
}
