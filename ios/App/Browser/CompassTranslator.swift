// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import BergamotKit
import CompassTranslate
import Foundation
import os

private let log = Logger(subsystem: "it.cipi1965.compass", category: "translate")

/// Compass's own engine on iOS (docs/design/translation.md, "iOS",
/// "Engines"): the desktop's Bergamot, built for iOS as
/// BergamotKit.xcframework (tools/translation/build_ios.py), with Firefox's
/// models from Compass's mirror, the desktop's manifest and checks
/// (`BergamotPacks`), into Application Support/Compass/Translation/. For
/// the languages Apple lacks, iOS 17, the simulator, and "Compass's own
/// models only".
@MainActor
final class CompassTranslator {
  static let shared = CompassTranslator()

  /// UserDefaults: the user agreed to download languages (once, as the
  /// desktop's `compass.translate.model_downloads_allowed`).
  static let downloadsAllowedKey = "TranslateModelDownloadsAllowed"

  let packs: BergamotPacks?
  private var engine: OpaquePointer?
  private var translators: [String: Handle] = [:]

  /// A translator's pointer, for the background queue (bk_translate() is
  /// thread-safe).
  struct Handle: @unchecked Sendable {
    let pointer: OpaquePointer
  }

  private init() {
    guard let url = Bundle.main.url(forResource: "models", withExtension: "json"),
      let data = try? Data(contentsOf: url),
      let manifest = try? TranslationManifest(data: data),
      let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
    else {
      packs = nil
      return
    }
    let root = support.appending(path: "Compass/Translation", directoryHint: .isDirectory)
    try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    // Models are downloaded again rather than backed up.
    var values = URLResourceValues()
    values.isExcludedFromBackup = true
    var excluded = root
    try? excluded.setResourceValues(values)
    packs = BergamotPacks(root: root, manifest: manifest, fetch: Self.fetch, decompress: { from, to in
      bk_zstd_decompress_file(from.path, to.path) == 1
    })
  }

  var downloadsAllowed: Bool {
    get { UserDefaults.standard.bool(forKey: Self.downloadsAllowedKey) }
    set { UserDefaults.standard.set(newValue, forKey: Self.downloadsAllowedKey) }
  }

  func translates(_ source: String, _ target: String) -> Bool {
    packs?.manifest.translates(source, target) ?? false
  }

  var languages: Set<String> { (packs?.manifest.languages ?? []).union(["en"]) }

  /// The packs `source` → `target` still needs.
  func missing(_ source: String, _ target: String) -> [String] {
    packs?.missing(source, target) ?? []
  }

  func install(_ languages: [String], progress: @escaping @Sendable (Int64, Int64) -> Void) async throws {
    guard let packs else { throw BergamotPacks.Failure.disk }
    try await packs.install(languages, progress: progress)
    reloadPackages()
  }

  func uninstall(_ language: String) {
    packs?.uninstall(language)
    reloadPackages()
  }

  /// The engine sees what's installed now; translators made before keep
  /// their models.
  private func reloadPackages() {
    translators.removeAll()
    guard let engine, let packs else { return }
    let packages = packs.packages
    withCStrings(packages.map(\.0)) { first in
      withCStrings(packages.map(\.1)) { second in
        withCStrings(packages.map(\.2)) { paths in
          bk_engine_set_packages(engine, first, second, paths, packages.count)
        }
      }
    }
  }

  /// Translates segments (HTML ones in Bergamot's HTML mode) off the main
  /// thread; nil for each that failed.
  func translate(_ segments: [PlainTextBatch.Segment], from source: String, to target: String) async -> [String?]? {
    guard let handle = translator(source, target) else { return nil }
    return await Task.detached(priority: .userInitiated) {
      segments.map { segment in
        segment.text.withCString { text -> String? in
          guard let out = bk_translate(handle.pointer, text, strlen(text), segment.html ? 1 : 0) else { return nil }
          defer { bk_free(out) }
          return String(cString: out)
        }
      }
    }.value
  }

  private func translator(_ source: String, _ target: String) -> Handle? {
    let key = "\(source)>\(target)"
    if let handle = translators[key] { return handle }
    if engine == nil {
      // Two workers: a phone's big cores, and the memory of two models.
      engine = bk_engine_create(2)
      reloadPackages()
    }
    guard let engine,
      let pointer = bk_translator_create(engine, source.lowercased(), target.lowercased())
    else {
      log.error("no translator for \(source, privacy: .public)→\(target, privacy: .public)")
      return nil
    }
    let handle = Handle(pointer: pointer)
    translators[key] = handle
    return handle
  }

  /// A download with progress, to a file of our own.
  private static let fetch: BergamotPacks.Fetch = { url, progress in
    let delegate = DownloadProgress(progress)
    let (temp, response) = try await URLSession.shared.download(from: url, delegate: delegate)
    guard (response as? HTTPURLResponse)?.statusCode == 200 else {
      try? FileManager.default.removeItem(at: temp)
      throw URLError(.badServerResponse)
    }
    let kept = FileManager.default.temporaryDirectory.appending(path: "translation-\(UUID().uuidString)")
    try FileManager.default.moveItem(at: temp, to: kept)
    return kept
  }
}

private final class DownloadProgress: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
  let report: @Sendable (Int64) -> Void
  init(_ report: @escaping @Sendable (Int64) -> Void) { self.report = report }

  func urlSession(
    _ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64
  ) {
    report(totalBytesWritten)
  }

  func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {}
}

/// C strings for the call, freed after.
private func withCStrings<R>(_ strings: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>) -> R) -> R {
  // A trailing nil, so the array is never empty.
  let pointers: [UnsafePointer<CChar>?] = strings.map { UnsafePointer(strdup($0)) } + [nil]
  defer { pointers.forEach { free(UnsafeMutablePointer(mutating: $0)) } }
  return pointers.withUnsafeBufferPointer { body($0.baseAddress!) }
}
