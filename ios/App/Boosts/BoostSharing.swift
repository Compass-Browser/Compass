// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import CompassShared
import SwiftUI
import UniformTypeIdentifiers

/// Sharing Boosts (docs/design/boosts.md, "Sharing"): a link
/// (https://compassbrowser.org/boost#…) or a .compassboost file out, and
/// the preview that adding one always goes through in. Claude has no part
/// in either.
@MainActor
enum BoostSharing {
  static let fileType = UTType(exportedAs: SharedBoost.typeIdentifier, conformingTo: .json)

  /// The link to `boost`, unless it would be longer than links are kept
  /// (then only the file is offered).
  static func link(for boost: Boost) -> URL? {
    guard let link = SharedBoost(boost).link(), link.absoluteString.utf8.count <= SharedBoost.maxLinkBytes else {
      return nil
    }
    return link
  }

  /// The share sheet with the link.
  static func shareLink(_ boost: Boost) {
    guard let link = link(for: boost) else { return shareFile(boost) }
    present(UIActivityViewController(activityItems: [link], applicationActivities: nil))
  }

  /// The share sheet with `<name>.compassboost`.
  static func shareFile(_ boost: Boost) {
    let shared = SharedBoost(boost)
    let directory = FileManager.default.temporaryDirectory.appendingPathComponent("Shared Boosts", isDirectory: true)
      .appendingPathComponent(UUID().uuidString, isDirectory: true)
    let file = directory.appendingPathComponent(shared.fileName)
    do {
      try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
      try shared.fileData.write(to: file, options: .atomic)
    } catch {
      return
    }
    let controller = UIActivityViewController(activityItems: [file], applicationActivities: nil)
    controller.completionWithItemsHandler = { _, _, _, _ in try? FileManager.default.removeItem(at: directory) }
    present(controller)
  }

  private static func present(_ controller: UIViewController) {
    // The iPad's popover needs somewhere to point (Presenter does it).
    Presenter.present(controller)
  }

  // MARK: Adding

  /// A preview is up: another link meanwhile (a page redirecting to one
  /// again and again) doesn't stack a second.
  private static var previewing = false

  /// A share link's fragment, from a tab (`incognito`: an Incognito one)
  /// or another app.
  static func preview(fragment: String, incognito: Bool = false) {
    preview(SharedBoost.decode(fragment: fragment), incognito: incognito)
  }

  /// A .compassboost file opened from Files, Mail or AirDrop.
  static func preview(file url: URL) {
    preview(read(file: url), incognito: false)
  }

  /// Reads a file (security-scoped when it's another app's), at most the
  /// size a shared Boost can have.
  static func read(file url: URL) -> Result<SharedBoost, SharedBoost.Failure> {
    let scoped = url.startAccessingSecurityScopedResource()
    defer { if scoped { url.stopAccessingSecurityScopedResource() } }
    var data: Data?
    var tooLarge = false
    NSFileCoordinator().coordinate(readingItemAt: url, options: .withoutChanges, error: nil) { url in
      guard let handle = try? FileHandle(forReadingFrom: url) else { return }
      defer { try? handle.close() }
      let read = (try? handle.read(upToCount: SharedBoost.maxJSONBytes + 1)) ?? Data()
      tooLarge = read.count > SharedBoost.maxJSONBytes
      data = read
    }
    if tooLarge { return .failure(.tooLarge) }
    guard let data else { return .failure(.notABoost) }
    return SharedBoost.decode(file: data)
  }

  private static func preview(_ result: Result<SharedBoost, SharedBoost.Failure>, incognito: Bool) {
    guard !previewing else { return }
    switch result {
    case .failure(let failure):
      let alert = UIAlertController(title: "Can't Add Boost", message: failure.description, preferredStyle: .alert)
      alert.addAction(UIAlertAction(title: "OK", style: .default) { _ in previewing = false })
      previewing = Presenter.present(alert)
    case .success(let shared):
      var controller: UIViewController?
      let view = NavigationStack {
        BoostImportPreview(shared: shared, incognito: incognito) {
          previewing = false
          controller?.dismiss(animated: true)
        }
      }
      let hosting = UIHostingController(rootView: view)
      // Swiped down: the same as Cancel.
      hosting.isModalInPresentation = false
      hosting.presentationController?.delegate = PreviewDismissal.shared
      controller = hosting
      previewing = Presenter.present(hosting)
    }
  }

  /// A preview swiped away.
  @MainActor
  private final class PreviewDismissal: NSObject, UIAdaptivePresentationControllerDelegate {
    static let shared = PreviewDismissal()
    func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
      BoostSharing.previewing = false
    }
  }
}

extension BoostStore {
  /// Adds a shared Boost: always a new one, by "shared" (never over a
  /// Boost that's here).
  func addShared(_ shared: SharedBoost) -> Result<Boost, SharedBoost.Failure> {
    switch list.resolveShared(shared) {
    case .failure(let failure): return .failure(failure)
    case .success(let boost):
      return put(boost) ? .success(boost) : .failure(.invalid(.full))
    }
  }
}

/// What a shared Boost would add, all of it: the name, the site, the CSS
/// and the JavaScript, with a warning when there's JavaScript. Only Add
/// adds it.
struct BoostImportPreview: View {
  let shared: SharedBoost
  /// Opened from an Incognito tab.
  var incognito = false
  /// After Add or Cancel.
  let done: () -> Void

  @State private var error: String?

  private var store: BoostStore { .shared }

  /// What a save would refuse, before Add.
  private var refusal: String? {
    if case .failure(let failure) = store.list.resolveShared(shared) { return failure.description }
    return nil
  }

  private var name: String {
    let name = shared.name.trimmingCharacters(in: .whitespacesAndNewlines)
    return name.isEmpty ? shared.match : name
  }

  private var site: String { BoostMatch(shared.match)?.pattern ?? shared.match }

  var body: some View {
    Form {
      Section {
        LabeledContent("Name", value: name)
          .accessibilityIdentifier("BoostImportName")
        LabeledContent("Site", value: site)
      } footer: {
        Text(
          "Add makes it a new Boost, turned on, in this device's Boosts"
            + (incognito
              ? ", not just in this Incognito tab: it stays after Incognito closes, and applies in your other tabs, not Incognito ones"
              : "")
            + ". When your spaces sync, it syncs with them like your other Boosts.")
      }
      if !shared.js.isEmpty {
        Section {
          Label {
            Text("This Boost runs JavaScript on \(site). Only add it if you trust whoever shared it.")
          } icon: {
            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
          }
          .accessibilityIdentifier("BoostImportJSWarning")
          Text(
            shared.jsWorld == .main
              ? "It runs in the page: the page's own scripts can see it, and it can use them."
              : "It runs apart from the page: it sees the page, but not the page's own scripts."
          )
          .font(.footnote)
          .foregroundStyle(.secondary)
        }
      }
      if !shared.css.isEmpty {
        Section("CSS") { code(shared.css) }
      }
      if !shared.js.isEmpty {
        Section(shared.jsWorld == .main ? "JavaScript (in the page)" : "JavaScript (apart from the page)") {
          code(shared.js)
        }
      }
      if let message = error ?? refusal {
        Section { Text(message).foregroundStyle(.red) }
      }
    }
    .navigationTitle("Add Shared Boost")
    .navigationBarTitleDisplayMode(.inline)
    .toolbar {
      ToolbarItem(placement: .cancellationAction) {
        Button("Cancel", action: done)
      }
      ToolbarItem(placement: .confirmationAction) {
        Button("Add", action: add)
          .disabled(refusal != nil)
          .accessibilityIdentifier("BoostImportAdd")
      }
    }
  }

  private func code(_ text: String) -> some View {
    Text(text)
      .font(.system(.footnote, design: .monospaced))
      .textSelection(.enabled)
      .frame(maxWidth: .infinity, alignment: .leading)
  }

  private func add() {
    switch store.addShared(shared) {
    case .success(let boost):
      AppServices.shared.active?.showMessage("Added “\(boost.name)” to Boosts")
      done()
    case .failure(let failure):
      error = failure.description
    }
  }
}

/// Settings → Boosts → Import from Link or File…: a link pasted in (the
/// clipboard isn't read), or a .compassboost file, then the preview.
struct BoostImportView: View {
  @Environment(\.dismiss) private var dismiss
  @State private var link = ""
  @State private var preview: SharedBoost?
  @State private var error: String?
  @State private var choosingFile = false

  var body: some View {
    Form {
      Section {
        TextField("https://compassbrowser.org/boost#…", text: $link, axis: .vertical)
          .textInputAutocapitalization(.never)
          .autocorrectionDisabled()
          .keyboardType(.URL)
          .lineLimit(1...4)
          .accessibilityIdentifier("BoostImportLink")
        Button("Show Boost", action: showLink)
          .disabled(link.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
          .accessibilityIdentifier("BoostImportShowLink")
      } header: {
        Text("Link")
      } footer: {
        Text("Paste a link someone shared to a Boost.")
      }
      Section {
        Button("Choose File…") { choosingFile = true }
      } footer: {
        Text("A .compassboost file, from Files or iCloud Drive.")
      }
      if let error {
        Section { Text(error).foregroundStyle(.red) }
      }
    }
    .navigationTitle("Import Boost")
    .navigationBarTitleDisplayMode(.inline)
    .toolbar {
      ToolbarItem(placement: .cancellationAction) {
        Button("Cancel") { dismiss() }
      }
    }
    .navigationDestination(item: $preview) { shared in
      BoostImportPreview(shared: shared) { dismiss() }
    }
    .fileImporter(isPresented: $choosingFile, allowedContentTypes: [BoostSharing.fileType, .json]) { result in
      guard case .success(let url) = result else { return }
      show(BoostSharing.read(file: url))
    }
  }

  private func showLink() {
    let text = link.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let url = URL(string: text), let fragment = AppLinks.boostShareFragment(url) else {
      error = "That isn't a link to a shared Boost. It starts with \(SharedBoost.linkPrefix)"
      return
    }
    show(SharedBoost.decode(fragment: fragment))
  }

  private func show(_ result: Result<SharedBoost, SharedBoost.Failure>) {
    switch result {
    case .success(let shared):
      error = nil
      preview = shared
    case .failure(let failure):
      error = failure.description
    }
  }
}

/// Share Link and Share File, for a menu.
struct BoostShareButtons: View {
  let boost: Boost

  var body: some View {
    if BoostSharing.link(for: boost) != nil {
      Button {
        BoostSharing.shareLink(boost)
      } label: {
        Label("Share Link", systemImage: "link")
      }
      .accessibilityIdentifier("BoostShareLink")
    }
    Button {
      BoostSharing.shareFile(boost)
    } label: {
      Label("Share File", systemImage: "doc")
    }
    .accessibilityIdentifier("BoostShareFile")
  }
}
