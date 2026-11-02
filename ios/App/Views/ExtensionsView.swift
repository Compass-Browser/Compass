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
        Text(record.enabled ? (webExtension?.displayVersion ?? "") : "Off")
          .font(.caption).foregroundStyle(.secondary)
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
          Button(extensions.fetching ? "Checking…" : "Update") { Task { await extensions.update(id) } }
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
    .navigationTitle(webExtension?.displayShortName ?? webExtension?.displayName ?? "Extension")
    .navigationBarTitleDisplayMode(.inline)
    .confirmationDialog("Remove this extension and its data?", isPresented: $confirmRemove, titleVisibility: .visible) {
      Button("Remove", role: .destructive) {
        extensions.remove(id)
        dismiss()
      }
    }
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
