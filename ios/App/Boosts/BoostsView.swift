// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import SwiftUI

/// Settings → Boosts (docs/design/boosts.md): every Boost, grouped by site,
/// with its switch; tap to edit, swipe to delete, + to add one.
struct BoostsView: View {
  @State private var editing: EditorTarget?

  private var store: BoostStore { .shared }

  var body: some View {
    List {
      if store.boosts.isEmpty {
        Section {
          Text("No Boosts yet.").foregroundStyle(.secondary)
        } footer: {
          Text(Self.about)
        }
      }
      ForEach(sites, id: \.self) { site in
        Section(site) {
          ForEach(store.boosts.filter { Self.site(of: $0) == site }) { boost in
            BoostRow(boost: boost) { editing = EditorTarget(boost: boost, match: boost.match) }
          }
          .onDelete { offsets in
            let shown = store.boosts.filter { Self.site(of: $0) == site }
            for id in offsets.map({ shown[$0].id }) { store.remove(id) }
          }
        }
      }
      if !store.boosts.isEmpty {
        Section {
        } footer: {
          Text(Self.about)
        }
      }
    }
    .navigationTitle("Boosts")
    .toolbar {
      Button {
        editing = EditorTarget(boost: nil, match: "")
      } label: {
        Image(systemName: "plus")
      }
      .accessibilityLabel("New Boost")
      .accessibilityIdentifier("BoostsAdd")
    }
    .sheet(item: $editing) { target in
      NavigationStack { BoostEditor(boost: target.boost, match: target.match) }
    }
  }

  static let about =
    "A Boost restyles a site with CSS, or changes it with JavaScript, each time you visit it, in every tab but Incognito ones. Boosts sync with your spaces, end-to-end encrypted. Changes show once the page loads again."

  private var sites: [String] {
    var seen = Set<String>()
    return store.boosts.map(Self.site).filter { seen.insert($0).inserted }.sorted()
  }

  static func site(of boost: Boost) -> String { boost.parsedMatch?.site ?? boost.match }
}

/// What the editor sheet opens on: a Boost, or a new one for `match`.
struct EditorTarget: Identifiable {
  let id = UUID()
  let boost: Boost?
  let match: String
}

/// A Boost in a list: its switch, name, pattern and what it carries.
private struct BoostRow: View {
  let boost: Boost
  let edit: () -> Void

  var body: some View {
    HStack {
      Button(action: edit) {
        VStack(alignment: .leading, spacing: 2) {
          Text(boost.name.isEmpty ? boost.match : boost.name).foregroundStyle(.primary)
          HStack(spacing: 6) {
            Text(boost.match)
            if !boost.css.isEmpty { Text("CSS") }
            if !boost.js.isEmpty { Text(boost.jsWorld == .main ? "JS (page)" : "JS") }
            if boost.author == .claude { Label("Claude", systemImage: "sparkles") }
          }
          .font(.caption)
          .foregroundStyle(.secondary)
          .lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .accessibilityIdentifier("BoostRow")
      Toggle(
        "Enabled",
        isOn: Binding(get: { boost.enabled }, set: { BoostStore.shared.setEnabled(boost.id, $0) })
      )
      .labelsHidden()
    }
  }
}

/// Edits a Boost, or makes one: its name, site pattern, CSS, JavaScript and
/// the world the JavaScript runs in.
struct BoostEditor: View {
  /// After a save (the page menu reloads the page).
  var onSaved: (() -> Void)?

  @Environment(\.dismiss) private var dismiss
  @State private var name: String
  @State private var match: String
  @State private var css: String
  @State private var js: String
  @State private var world: Boost.World
  @State private var enabled: Bool
  @State private var error: String?
  @State private var confirmingDelete = false
  private let original: Boost?

  init(boost: Boost?, match: String, onSaved: (() -> Void)? = nil) {
    original = boost
    self.onSaved = onSaved
    _name = State(initialValue: boost?.name ?? BoostMatch(match)?.site ?? "")
    _match = State(initialValue: boost?.match ?? match)
    _css = State(initialValue: boost?.css ?? "")
    _js = State(initialValue: boost?.js ?? "")
    _world = State(initialValue: boost?.jsWorld ?? .isolated)
    _enabled = State(initialValue: boost?.enabled ?? true)
  }

  var body: some View {
    Form {
      Section {
        TextField("Name", text: $name)
          .accessibilityIdentifier("BoostName")
        TextField("example.com", text: $match)
          .textInputAutocapitalization(.never)
          .autocorrectionDisabled()
          .keyboardType(.URL)
          .accessibilityIdentifier("BoostMatch")
        Toggle("Enabled", isOn: $enabled)
      } footer: {
        Text("“example.com” is that site and www.example.com; “*.example.com” adds every subdomain. A path after it, as in “github.com/settings”, limits the Boost to the pages under it.")
      }
      Section("CSS") {
        code($css, identifier: "BoostCSS")
      }
      Section {
        code($js, identifier: "BoostJS")
        Picker("Runs", selection: $world) {
          Text("Apart from the Page").tag(Boost.World.isolated)
          Text("In the Page").tag(Boost.World.main)
        }
      } header: {
        Text("JavaScript")
      } footer: {
        Text("Runs once the page has loaded. Apart from the page, it sees the page but not the page's own scripts; in the page, it can use them, and they can see it.")
      }
      if let original {
        Section {
          if original.author == .claude {
            Label("Made by Claude", systemImage: "sparkles").foregroundStyle(.secondary)
          }
          Button("Delete Boost", role: .destructive) { confirmingDelete = true }
        }
      }
      if let error {
        Section { Text(error).foregroundStyle(.red) }
      }
    }
    .navigationTitle(original == nil ? "New Boost" : "Edit Boost")
    .navigationBarTitleDisplayMode(.inline)
    .toolbar {
      ToolbarItem(placement: .cancellationAction) {
        Button("Cancel") { dismiss() }
      }
      ToolbarItem(placement: .confirmationAction) {
        Button("Save", action: save)
          .disabled(match.trimmingCharacters(in: .whitespaces).isEmpty)
          .accessibilityIdentifier("BoostSave")
      }
    }
    .confirmationDialog(
      "Delete this Boost on all your devices?", isPresented: $confirmingDelete, titleVisibility: .visible
    ) {
      Button("Delete", role: .destructive) {
        if let original { BoostStore.shared.remove(original.id) }
        onSaved?()
        dismiss()
      }
    }
  }

  private func code(_ text: Binding<String>, identifier: String) -> some View {
    TextEditor(text: text)
      .font(.system(.footnote, design: .monospaced))
      .textInputAutocapitalization(.never)
      .autocorrectionDisabled()
      .frame(minHeight: 140)
      .accessibilityIdentifier(identifier)
  }

  private func save() {
    let draft = BoostDraft(
      id: original?.id, name: name.trimmingCharacters(in: .whitespaces).isEmpty ? match : name, match: match,
      css: css, js: js, jsWorld: world.rawValue, enabled: enabled)
    switch BoostStore.shared.save(draft) {
    case .success:
      onSaved?()
      dismiss()
    case .failure(let failure):
      error = failure.description
    }
  }
}

/// The page menu's "Boost This Site": the Boosts on the shown page's
/// site, to edit, and a new one for it. A save reloads the page.
struct SiteBoostsView: View {
  let url: URL
  let reload: () -> Void

  @Environment(\.dismiss) private var dismiss
  @State private var editing: EditorTarget?

  private var store: BoostStore { .shared }

  var body: some View {
    let host = url.host ?? ""
    let boosts = store.list.forHost(host)
    let suggested = BoostMatch.suggested(for: url) ?? host
    List {
      Section {
        ForEach(boosts) { boost in
          BoostRow(boost: boost) { editing = EditorTarget(boost: boost, match: boost.match) }
        }
        .onDelete { offsets in
          for id in offsets.map({ boosts[$0].id }) { store.remove(id) }
          reload()
        }
        Button {
          editing = EditorTarget(boost: nil, match: suggested)
        } label: {
          Label("New Boost for \(suggested)", systemImage: "plus")
        }
        .accessibilityIdentifier("BoostsNewForSite")
      } footer: {
        Text(BoostsView.about)
      }
    }
    .navigationTitle(suggested)
    .navigationBarTitleDisplayMode(.inline)
    .toolbar {
      ToolbarItem(placement: .confirmationAction) {
        Button("Done") { dismiss() }
      }
    }
    .sheet(item: $editing) { target in
      NavigationStack { BoostEditor(boost: target.boost, match: target.match, onSaved: reload) }
    }
  }
}
