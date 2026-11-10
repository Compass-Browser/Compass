// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassLiveFolders
import CompassModel
import SwiftUI
import UIKit

/// What the live folder sheet is for: a new folder in a Pinned parent, or
/// a folder's source (Edit Source…).
struct LiveEditorTarget: Identifiable {
  enum Mode {
    case create(parent: String)
    case edit(folder: String)
  }

  let id = UUID()
  let mode: Mode
}

/// A plugin's icon: an SF Symbol for the built-ins (their icons are SVG,
/// which UIKit doesn't draw), an installed plugin's PNG, or a bolt.
struct LivePluginIcon: View {
  let plugin: LivePlugin?
  var size: CGFloat = 15

  static func symbol(for id: String?) -> String {
    switch id {
    case "compass.rss": "dot.radiowaves.up.forward"
    case "compass.github": "chevron.left.forwardslash.chevron.right"
    case "compass.web-list": "list.bullet.rectangle"
    case "compass.calendar": "calendar"
    default: "bolt.fill"
    }
  }

  var body: some View {
    if let data = plugin?.icon, !(plugin?.isBuiltin ?? true), let image = UIImage(data: data) {
      Image(uiImage: image).resizable().scaledToFit().frame(width: size + 2, height: size + 2)
    } else {
      Image(systemName: Self.symbol(for: plugin?.id)).font(.system(size: size))
    }
  }
}

/// New Live Folder and Edit Source: the plugin picker, then the form built
/// from the plugin's manifest (settings, secrets, the cookie grant), and
/// installing a plugin from its manifest's address after reviewing what it
/// reaches (the desktop's sidebar editor).
struct LiveFolderEditor: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  let target: LiveEditorTarget

  @State private var plugin: LivePlugin?
  @State private var name = ""
  @State private var text: [String: String] = [:]
  @State private var flags: [String: Bool] = [:]
  @State private var secrets: [String: String] = [:]
  @State private var grantCookies = false
  @State private var error: String?
  @State private var installing = false
  @State private var started = false

  private var live: LiveFolders { app.liveFolders }
  private var folder: SidebarNode? {
    if case .edit(let id) = target.mode { return app.store.node(id: id) }
    return nil
  }

  var body: some View {
    NavigationStack {
      Group {
        if installing {
          LivePluginInstallView(
            initialAddress: folder?.liveSource?.pluginURL ?? "",
            expectedHash: folder?.liveSource?.pluginSHA256 ?? ""
          ) { installed in
            installing = false
            if let installed, let p = live.plugin(id: installed.id) { choose(p) }
          }
        } else if let plugin {
          form(plugin)
        } else if let folder {
          missing(folder)
        } else {
          picker
        }
      }
      .navigationBarTitleDisplayMode(.inline)
    }
    .presentationDragIndicator(.visible)
    .onAppear {
      guard !started else { return }
      started = true
      if let folder, let p = live.plugin(for: folder) { choose(p, config: folder.liveSource?.config ?? [:]) }
    }
  }

  // MARK: Picker

  private var picker: some View {
    List {
      Section {
        ForEach(live.plugins, id: \.id) { p in
          Button { choose(p) } label: {
            HStack(spacing: 12) {
              LivePluginIcon(plugin: p, size: 18).frame(width: 28)
              VStack(alignment: .leading, spacing: 2) {
                Text(p.manifest.name).foregroundStyle(.primary)
                if !p.manifest.description.isEmpty {
                  Text(p.manifest.description).font(.footnote).foregroundStyle(.secondary)
                }
              }
            }
          }
          .swipeActions {
            if !p.isBuiltin {
              Button("Remove", role: .destructive) { live.remove(p) }
            }
          }
        }
      } footer: {
        Text("Live folders fill themselves from a source and keep up with it. Each device runs the plugin; only the folder's settings sync.")
      }
      Section {
        Button("Install a Plugin…", systemImage: "square.and.arrow.down") { installing = true }
      }
    }
    .navigationTitle("New Live Folder")
    .toolbar {
      ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
    }
  }

  /// A synced folder whose plugin this device doesn't have.
  private func missing(_ folder: SidebarNode) -> some View {
    Form {
      Section {
        Text("This folder's plugin (\(folder.liveSource?.pluginID ?? "")) isn't installed on this device.")
        if !(folder.liveSource?.pluginURL ?? "").isEmpty {
          Button("Install the Plugin…") { installing = true }
        }
      }
    }
    .navigationTitle("Edit Source")
    .toolbar {
      ToolbarItem(placement: .cancellationAction) { Button("Close") { dismiss() } }
    }
  }

  // MARK: Form

  private func choose(_ p: LivePlugin, config: [String: Any] = [:]) {
    plugin = p
    error = nil
    text = [:]
    flags = [:]
    for field in p.manifest.config {
      let value = config[field.key].map { LiveJSON(object: $0) } ?? field.default
      if field.type == "bool" {
        if case .bool(let b) = value { flags[field.key] = b } else { flags[field.key] = false }
      } else {
        text[field.key] = value?.text ?? ""
      }
    }
    if let folder { grantCookies = live.cookiesGranted(folder) }
  }

  private func form(_ p: LivePlugin) -> some View {
    let profile = folder.map(app.store.profile(of:)) ?? targetParent.map(app.store.profile(of:)) ?? ""
    return Form {
      if folder == nil {
        Section {
          TextField(p.manifest.name, text: $name)
        } header: {
          Text("Name")
        }
      }
      if !p.manifest.config.isEmpty {
        Section {
          ForEach(p.manifest.config, id: \.key) { field($0) }
        } header: {
          Text("Settings")
        } footer: {
          Text("Synced with the folder.")
        }
      }
      ForEach(p.manifest.secrets, id: \.key) { s in
        Section {
          SecureField(
            live.secret(s.key, plugin: p.id, profile: profile) == nil ? (s.optional ? "Optional" : "Required")
              : "Saved: leave blank to keep",
            text: Binding(get: { secrets[s.key] ?? "" }, set: { secrets[s.key] = $0 })
          )
          .textInputAutocapitalization(.never)
          .autocorrectionDisabled()
        } header: {
          Text(s.label)
        } footer: {
          Text(s.help.isEmpty ? "Kept on this device only." : "\(s.help) Kept on this device only.")
        }
      }
      if !p.manifest.cookies.isEmpty {
        Section {
          Toggle("Send My Cookies", isOn: $grantCookies)
        } footer: {
          Text("Sends your cookies for \(describe(p.manifest.cookies)), so the folder sees what you see signed in. Only for this folder, on this device.")
        }
      }
      if let error {
        Section { Text(error).foregroundStyle(.red) }
      }
    }
    .navigationTitle(folder == nil ? p.manifest.name : "Edit Source")
    .toolbar {
      ToolbarItem(placement: .cancellationAction) {
        if folder == nil {
          Button("Back") { plugin = nil }
        } else {
          Button("Cancel") { dismiss() }
        }
      }
      ToolbarItem(placement: .confirmationAction) {
        Button(folder == nil ? "Create" : "Save") { save(p) }
      }
    }
  }

  @ViewBuilder private func field(_ f: LiveManifest.Field) -> some View {
    let label = f.optional ? "\(f.label) (optional)" : f.label
    switch f.type {
    case "bool":
      Toggle(label, isOn: Binding(get: { flags[f.key] ?? false }, set: { flags[f.key] = $0 }))
    case "select":
      Picker(label, selection: Binding(get: { text[f.key] ?? "" }, set: { text[f.key] = $0 })) {
        ForEach(f.options ?? [], id: \.value) { Text($0.label).tag($0.value) }
      }
    default:
      VStack(alignment: .leading, spacing: 4) {
        Text(label).font(.footnote).foregroundStyle(.secondary)
        TextField(f.placeholder ?? "", text: Binding(get: { text[f.key] ?? "" }, set: { text[f.key] = $0 }))
          .keyboardType(f.type == "url" ? .URL : f.type == "number" ? .numbersAndPunctuation : .default)
          .textInputAutocapitalization(f.type == "string" ? .sentences : .never)
          .autocorrectionDisabled(f.type != "string")
        if let help = f.help, !help.isEmpty {
          Text(help).font(.footnote).foregroundStyle(.secondary)
        }
      }
    }
  }

  private var targetParent: SidebarNode? {
    if case .create(let id) = target.mode { return app.store.node(id: id) }
    return nil
  }

  /// The form's values as the folder's config.
  private func config(_ p: LivePlugin) -> [String: Any] {
    var out: [String: Any] = [:]
    for f in p.manifest.config {
      switch f.type {
      case "bool": out[f.key] = flags[f.key] ?? false
      case "number":
        let s = (text[f.key] ?? "").trimmingCharacters(in: .whitespaces)
        if let n = Double(s) { out[f.key] = n == n.rounded() ? Int(n) as Any : n } else if !s.isEmpty { out[f.key] = s }
      default:
        let s = (text[f.key] ?? "").trimmingCharacters(in: .whitespaces)
        if !s.isEmpty { out[f.key] = s }
      }
    }
    return out
  }

  private func save(_ p: LivePlugin) {
    let values = config(p)
    // The plugin's own rules first, so the message is the form's.
    if let rules = live.rules {
      do {
        _ = try rules.config(p.manifest, LiveSource.json(values))
      } catch let e as LiveError {
        error = e.message
        return
      } catch {}
    }
    if let folder {
      live.update(folder, plugin: p, config: values, secrets: secrets, grantCookies: grantCookies)
    } else if let parent = targetParent {
      guard
        let created = live.create(
          plugin: p, name: name, config: values, secrets: secrets, grantCookies: grantCookies, in: parent)
      else {
        error = "A live folder can't go there."
        return
      }
      if created.collapsed { app.store.setCollapsed(created, false) }
    }
    dismiss()
  }

  private func describe(_ hosts: [String]) -> String {
    hosts.map { $0.hasPrefix("{") ? "the address you enter" : $0 }.joined(separator: ", ")
  }
}

/// Install a plugin from its manifest's address: fetch, review what it
/// reaches and asks for, then install.
struct LivePluginInstallView: View {
  @Environment(AppModel.self) private var app
  @State var address: String
  let expectedHash: String
  let done: (LivePluginPackage?) -> Void
  @State private var preview: LivePluginPackage?
  @State private var error: String?
  @State private var busy = false

  init(initialAddress: String, expectedHash: String, done: @escaping (LivePluginPackage?) -> Void) {
    _address = State(initialValue: initialAddress)
    self.expectedHash = expectedHash
    self.done = done
  }

  var body: some View {
    Form {
      Section {
        TextField("https://example.com/plugin/manifest.json", text: $address)
          .keyboardType(.URL)
          .textInputAutocapitalization(.never)
          .autocorrectionDisabled()
          .onChange(of: address) { preview = nil }
      } header: {
        Text("Address of the plugin's manifest.json")
      }
      if let p = preview {
        Section {
          LabeledContent(p.manifest.name, value: p.manifest.version)
          if !p.manifest.description.isEmpty { Text(p.manifest.description).font(.footnote) }
          LabeledContent("Reaches", value: describe(p.manifest.hosts))
          if !p.manifest.cookies.isEmpty {
            LabeledContent("Can ask for your cookies for", value: describe(p.manifest.cookies))
          }
          if !p.manifest.secrets.isEmpty {
            LabeledContent("Asks for", value: p.manifest.secrets.map(\.label).joined(separator: ", "))
          }
        } header: {
          Text("Review")
        } footer: {
          Text("From \(p.source.absoluteString). Install only plugins you trust: a plugin sees the pages it reaches.")
        }
      }
      if let error {
        Section { Text(error).foregroundStyle(.red) }
      }
    }
    .navigationTitle("Install a Plugin")
    .toolbar {
      ToolbarItem(placement: .cancellationAction) { Button("Cancel") { done(nil) } }
      ToolbarItem(placement: .confirmationAction) {
        if busy {
          ProgressView()
        } else if let p = preview {
          Button("Install") { install(p) }
        } else {
          Button("Review") { review() }.disabled(address.isEmpty)
        }
      }
    }
  }

  private func review() {
    busy = true
    error = nil
    Task {
      do {
        let package = try await app.liveFolders.fetchPackage(address)
        if !expectedHash.isEmpty && package.sha256 != expectedHash {
          error = "The plugin at that address isn't the one this folder was made with (it has changed since)."
        } else {
          preview = package
        }
      } catch let e as LiveError {
        error = e.message
      } catch {
        self.error = error.localizedDescription
      }
      busy = false
    }
  }

  private func install(_ package: LivePluginPackage) {
    do {
      try app.liveFolders.install(package)
      done(package)
    } catch {
      self.error = "It couldn't be installed: \(error.localizedDescription)"
    }
  }

  private func describe(_ hosts: [String]) -> String {
    hosts.isEmpty ? "nothing" : hosts.map { $0.hasPrefix("{") ? "the address you enter" : $0 }.joined(separator: ", ")
  }
}

/// A live folder's state at the end of its row: a spinner while it
/// refreshes, a warning after a failed refresh, and while it's closed, how
/// many of its entries are new.
struct LiveFolderStatus: View {
  @Environment(AppModel.self) private var app
  let folder: SidebarNode

  var body: some View {
    let state = app.liveFolders.states[folder.id]
    HStack(spacing: 6) {
      if state?.refreshing == true {
        ProgressView().controlSize(.mini)
      } else if let error = state?.error {
        Image(systemName: "exclamationmark.triangle.fill")
          .font(.system(size: 12))
          .foregroundStyle(.orange)
          .accessibilityLabel(error.message)
      }
      if folder.collapsed, folder.unseenCount > 0 {
        Text("\(folder.unseenCount)")
          .font(.system(size: 11, weight: .semibold))
          .monospacedDigit()
          .foregroundStyle(.white)
          .padding(.horizontal, 6)
          .frame(minWidth: 18, minHeight: 18)
          .background(Color.accentColor, in: Capsule())
          .accessibilityLabel("\(folder.unseenCount) new")
      }
    }
  }
}

/// A live folder's own items in its menu (the desktop's context menu):
/// Refresh, Edit Source…, Mark All Seen, Convert to Normal Folder, and the
/// last refresh's error.
struct LiveFolderMenu: View {
  @Environment(AppModel.self) private var app
  let folder: SidebarNode
  /// New Live Folder's icon.
  static let newSymbol = "bolt.horizontal.circle"

  var body: some View {
    Section {
      if let error = app.liveFolders.states[folder.id]?.error {
        Text(error.message)
      }
      Button("Refresh", systemImage: "arrow.clockwise") { app.liveFolders.refresh(folder) }
      Button("Edit Source…", systemImage: "slider.horizontal.3") {
        app.liveEditor = LiveEditorTarget(mode: .edit(folder: folder.id))
      }
      if folder.unseenCount > 0 {
        Button("Mark All Seen", systemImage: "checkmark.circle") { app.store.markAllSeen(folder) }
      }
      Button("Convert to Normal Folder", systemImage: "folder") {
        withAnimation(SidebarStyle.motion) { app.liveFolders.convertToPlain(folder) }
      }
    }
  }
}
