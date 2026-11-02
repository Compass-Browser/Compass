// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI
import UniformTypeIdentifiers
import WebKit

/// Settings → Extensions: the added extensions (on/off, details), and
/// adding one from a store link or a file.
@available(iOS 18.4, *)
struct ExtensionsView: View {
  @Environment(AppModel.self) private var app
  @State private var link = ""
  @State private var importing = false
  @State private var checking = false

  var body: some View {
    let extensions = app.extensions
    let _ = extensions.revision
    Form {
      if !extensions.records.isEmpty {
        Section {
          ForEach(extensions.records) { record in
            NavigationLink { ExtensionDetailView(id: record.id) } label: {
              ExtensionRow(record: record)
            }
          }
          .onDelete { offsets in
            for index in offsets { extensions.remove(extensions.records[index].id) }
          }
        }
      }
      let installed = Set(extensions.records.map(\.id))
      let fromMac = app.sync.syncedExtensions.filter { $0.enabled && $0.isFromWebStore && !installed.contains($0.id) }
      if !fromMac.isEmpty {
        Section {
          ForEach(fromMac) { synced in
            HStack {
              VStack(alignment: .leading, spacing: 2) {
                Text(synced.name.isEmpty ? synced.id : synced.name)
                Text("Chrome Web Store").font(.caption).foregroundStyle(.secondary)
              }
              Spacer()
              Button("Add") { Task { await extensions.fetch(.chromeWebStore(synced.id)) } }
                .buttonStyle(.bordered)
                .disabled(extensions.fetching)
            }
          }
        } header: {
          Text("On Your Other Devices")
        } footer: {
          Text("Extensions your Mac syncs. Each one asks before it's added here.")
        }
      }
      Section {
        HStack {
          TextField("Store link or extension ID", text: $link)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .keyboardType(.URL)
            .submitLabel(.go)
            .onSubmit(add)
          if extensions.fetching {
            ProgressView()
          } else {
            Button("Add", action: add).disabled(ExtensionSource(link) == nil)
          }
        }
        Button("Open the Chrome Web Store") { open("https://chromewebstore.google.com/category/extensions") }
        Button("Open Firefox Add-ons") { open("https://addons.mozilla.org/firefox/extensions/") }
        Button("Add from Files…") { importing = true }
        if !extensions.records.isEmpty {
          Button(checking ? "Checking for Updates…" : "Check for Updates") {
            checking = true
            Task {
              await extensions.checkForUpdates(force: true)
              checking = false
            }
          }
          .disabled(checking)
        }
      } header: {
        Text("Add an Extension")
      } footer: {
        Text(
          "Paste a Chrome Web Store or Firefox Add-ons link, or open a store, go to an extension's page and choose “Add to Compass” in the menu. Extensions run on every profile; some features of desktop extensions aren't available on iPhone."
        )
      }
      if let error = extensions.installError {
        Section { Text(error).foregroundStyle(.red) }
      }
    }
    .navigationTitle("Extensions")
    .fileImporter(
      isPresented: $importing,
      allowedContentTypes: [.zip, UTType(filenameExtension: "crx") ?? .data, UTType(filenameExtension: "xpi") ?? .data]
    ) { result in
      if case .success(let url) = result { Task { await extensions.fetch(file: url) } }
    }
  }

  private func add() {
    guard let source = ExtensionSource(link) else { return }
    link = ""
    Task { await app.extensions.fetch(source) }
  }

  private func open(_ url: String) {
    app.showingSettings = false
    Task {
      try? await Task.sleep(for: .milliseconds(450))
      app.open(url)
    }
  }
}

@available(iOS 18.4, *)
private struct ExtensionRow: View {
  @Environment(AppModel.self) private var app
  let record: WebExtensions.Record

  var body: some View {
    let webExtension = app.extensions.webExtension(record.id)
    HStack(spacing: 12) {
      ExtensionIcon(image: webExtension?.icon(for: CGSize(width: 32, height: 32)), size: 32)
      VStack(alignment: .leading, spacing: 2) {
        Text(webExtension?.displayName ?? "Extension")
        Text(record.availableUpdate.map { "Update available: \($0)" }
          ?? (record.enabled ? (webExtension?.displayVersion ?? "") : "Off"))
          .font(.caption).foregroundStyle(record.availableUpdate == nil ? Color.secondary : Color.accentColor)
      }
    }
  }
}

/// One extension: on/off, what it can do, options, update and remove.
@available(iOS 18.4, *)
struct ExtensionDetailView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  let id: String
  @State private var confirmRemove = false
  @State private var unlockSaved = false

  var body: some View {
    let extensions = app.extensions
    let _ = extensions.revision
    let record = extensions.records.first { $0.id == id }
    let webExtension = extensions.webExtension(id)
    Form {
      Section {
        HStack(spacing: 14) {
          ExtensionIcon(image: webExtension?.icon(for: CGSize(width: 56, height: 56)), size: 56)
          VStack(alignment: .leading, spacing: 4) {
            Text(webExtension?.displayName ?? "Extension").font(.headline)
            Text([webExtension?.displayVersion, record?.source.storeName].compactMap { $0 }.joined(separator: " · "))
              .font(.caption).foregroundStyle(.secondary)
          }
        }
        if let description = webExtension?.displayDescription, !description.isEmpty {
          Text(description).font(.callout).foregroundStyle(.secondary)
        }
        Toggle("On", isOn: Binding(
          get: { record?.enabled ?? false },
          set: { extensions.setEnabled(id, $0) }))
      }
      Section {
        Toggle("Allow in Incognito", isOn: Binding(
          get: { record?.allowIncognito == true },
          set: { extensions.setAllowIncognito(id, $0) }))
      } footer: {
        Text("Compass can't stop extensions from recording what you do in Incognito.")
      }
      if unlockSaved {
        Section {
          Toggle("Unlock with \(ExtensionUnlock.biometryName)", isOn: Binding(
            get: { unlockSaved },
            set: { on in
              if !on {
                ExtensionUnlock.forget(id)
                unlockSaved = false
              }
            }))
        } footer: {
          Text("Compass fills in the password you saved when it asks to unlock. Turn off to forget it; type the password again to turn it back on.")
        }
      }
      if let record, let webExtension, !webExtension.allRequestedMatchPatterns.isEmpty {
        WebsiteAccessSection(id: id, record: record)
      }
      if let webExtension {
        let lines = ExtensionPermissions.summary(webExtension)
        if !lines.isEmpty {
          Section("It Can") {
            ForEach(lines, id: \.self) { Text($0) }
          }
        }
        let errors = webExtension.errors + (extensions.context(id)?.errors ?? [])
        if !errors.isEmpty {
          Section("Problems") {
            ForEach(Array(errors.enumerated()), id: \.offset) { _, error in
              Text(error.localizedDescription).font(.caption).foregroundStyle(.secondary)
            }
          }
        }
      }
      Section {
        if extensions.context(id)?.optionsPageURL != nil {
          Button("Options") { extensions.openOptions(id) }
        }
        if let record, record.source != .file {
          let title = record.availableUpdate.map { "Review Update to \($0)" } ?? "Update"
          Button(extensions.fetching ? "Checking…" : title) { Task { await extensions.update(id) } }
            .disabled(extensions.fetching)
          if let page = record.source.pageURL {
            Button("Show in \(record.source.storeName)") {
              app.showingSettings = false
              Task {
                try? await Task.sleep(for: .milliseconds(450))
                app.open(page.absoluteString)
              }
            }
          }
        }
        Button("Remove", role: .destructive) { confirmRemove = true }
      }
    }
    .onAppear { unlockSaved = ExtensionUnlock.isSaved(id) }
    .navigationTitle(webExtension?.displayShortName ?? webExtension?.displayName ?? "Extension")
    .navigationBarTitleDisplayMode(.inline)
    .confirmationDialog("Remove this extension and its data?", isPresented: $confirmRemove, titleVisibility: .visible) {
      Button("Remove", role: .destructive) {
        ExtensionUnlock.forget(id)
        extensions.remove(id)
        dismiss()
      }
    }
  }
}

/// Website access: every site the extension asks for, or only the sites
/// listed here (and their subdomains).
@available(iOS 18.4, *)
private struct WebsiteAccessSection: View {
  @Environment(AppModel.self) private var app
  let id: String
  let record: WebExtensions.Record
  @State private var newSite = ""

  var body: some View {
    let extensions = app.extensions
    Section {
      Picker("Website Access", selection: Binding(
        get: { record.onlySites == nil },
        set: { extensions.setOnlySites(id, $0 ? nil : (record.onlySites ?? [])) })) {
        Text("All Sites It Asks For").tag(true)
        Text("Only These Sites").tag(false)
      }
      if let sites = record.onlySites {
        ForEach(sites, id: \.self) { Text($0) }
          .onDelete { offsets in extensions.setOnlySites(id, sites.enumerated().filter { !offsets.contains($0.offset) }.map(\.element)) }
        HStack {
          TextField("example.com", text: $newSite)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .keyboardType(.URL)
            .onSubmit(add)
          Button("Add", action: add).disabled(Self.host(newSite) == nil)
        }
      }
    } header: {
      Text("Website Access")
    } footer: {
      Text(record.onlySites == nil
        ? "It runs on every site it asks for."
        : "It runs only on these sites and their subdomains. Long-press its button in the page menu to allow the site you're on.")
    }
  }

  private func add() {
    guard let host = Self.host(newSite) else { return }
    newSite = ""
    app.extensions.setOnlySites(id, (record.onlySites ?? []) + [host])
  }

  static func host(_ text: String) -> String? {
    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    guard !trimmed.isEmpty else { return nil }
    let url = URL(string: trimmed.contains("://") ? trimmed : "https://" + trimmed)
    guard let host = url?.host(), host.contains(".") else { return nil }
    return host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
  }
}

/// "Add “Name”?": what the extension can do, before it runs. Presented by
/// WebExtensions over whatever is up when a download is read.
@available(iOS 18.4, *)
struct ExtensionInstallSheet: View {
  let extensions: WebExtensions
  let pending: WebExtensions.Pending
  let done: () -> Void

  var body: some View {
    let webExtension = pending.webExtension as! WKWebExtension
    let updating = extensions.records.contains { $0.source == pending.source && pending.source != .file }
    let lines = ExtensionPermissions.summary(webExtension)
    VStack(spacing: 18) {
      ExtensionIcon(image: webExtension.icon(for: CGSize(width: 64, height: 64)), size: 64)
        .padding(.top, 28)
      VStack(spacing: 6) {
        Text("\(updating ? "Update" : "Add") “\(webExtension.displayName ?? "Extension")”?")
          .font(.title3.weight(.semibold))
          .multilineTextAlignment(.center)
        Text([webExtension.displayVersion, pending.source.storeName].compactMap { $0 }.joined(separator: " · "))
          .font(.caption).foregroundStyle(.secondary)
      }
      if !lines.isEmpty {
        VStack(alignment: .leading, spacing: 8) {
          Text("It can:").font(.subheadline.weight(.semibold))
          ForEach(lines, id: \.self) { line in
            Label(line, systemImage: "checkmark.shield").font(.subheadline)
          }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(14)
        .background(Color(.secondarySystemBackground), in: RoundedRectangle(cornerRadius: 14))
      }
      if !webExtension.errors.isEmpty {
        Text("Some parts of it may not work on iPhone.").font(.caption).foregroundStyle(.secondary)
      }
      Spacer(minLength: 0)
      Button {
        Task { await extensions.confirmPending() }
        done()
      } label: {
        Text(updating ? "Update" : "Add Extension").font(.headline).frame(maxWidth: .infinity).frame(height: 50)
      }
      .buttonStyle(.borderedProminent)
      .buttonBorderShape(.capsule)
      .accessibilityIdentifier("AddExtension")
      Button("Cancel") {
        extensions.cancelPending()
        done()
      }
      .padding(.bottom, 8)
    }
    .padding(.horizontal, 24)
  }
}

/// An extension's icon, or a puzzle piece when it has none.
struct ExtensionIcon: View {
  let image: UIImage?
  let size: CGFloat

  var body: some View {
    if let image {
      Image(uiImage: image).resizable().scaledToFit().frame(width: size, height: size)
    } else {
      Image(systemName: "puzzlepiece.extension.fill")
        .font(.system(size: size * 0.5))
        .foregroundStyle(.secondary)
        .frame(width: size, height: size)
        .background(Color(.tertiarySystemFill), in: RoundedRectangle(cornerRadius: size * 0.22))
    }
  }
}
