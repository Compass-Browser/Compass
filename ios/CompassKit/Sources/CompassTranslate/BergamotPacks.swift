// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CryptoKit
import Foundation

/// The desktop's model manifest (browser/translate/models.json, bundled):
/// Firefox's models on Compass's mirror, each direction's files with their
/// sizes and SHA-256 hashes, compressed and not (docs/design/
/// translation.md, "Firefox's models, from Compass's mirror").
public struct TranslationManifest: Decodable, Sendable {
  public struct File: Decodable, Sendable, Equatable {
    /// "model", "vocab", "srcvocab" or "trgvocab".
    public let type: String
    public let location: String
    public let size: Int64
    public let hash: String
    public let decompressedSize: Int64
    public let decompressedHash: String

    enum CodingKeys: String, CodingKey {
      case type, location, size, hash
      case decompressedSize = "decompressed_size"
      case decompressedHash = "decompressed_hash"
    }

    public init(type: String, location: String, size: Int64, hash: String, decompressedSize: Int64, decompressedHash: String) {
      self.type = type
      self.location = location
      self.size = size
      self.hash = hash
      self.decompressedSize = decompressedSize
      self.decompressedHash = decompressedHash
    }
  }

  public struct Direction: Decodable, Sendable {
    public let files: [File]
    public init(files: [File]) { self.files = files }
  }

  /// Where `location`s are, ending in "/".
  public let attachments: String
  /// By "<from>-<to>", one side always English ("de-en", "en-zh-Hans").
  public let directions: [String: Direction]

  public init(attachments: String, directions: [String: Direction]) {
    self.attachments = attachments
    self.directions = directions
  }

  public init(data: Data) throws {
    self = try JSONDecoder().decode(Self.self, from: data)
  }

  /// The file name the engine looks for (engine.h), or nil for a type it
  /// doesn't use.
  public static func fileName(_ type: String) -> String? {
    switch type {
    case "model": "model.bin"
    case "vocab": "vocab.spm"
    case "srcvocab": "srcvocab.spm"
    case "trgvocab": "trgvocab.spm"
    default: nil
    }
  }

  /// The languages other than English Bergamot translates from or into.
  public var languages: Set<String> {
    Set(directions.keys.compactMap { key in
      if key.hasPrefix("en-") { return String(key.dropFirst(3)) }
      if key.hasSuffix("-en") { return String(key.dropLast(3)) }
      return nil
    })
  }

  /// Whether `source` translates into `target`, directly or through
  /// English.
  public func translates(_ source: String, _ target: String) -> Bool {
    guard source != target else { return false }
    let into = source == "en" || directions["\(source)-en"] != nil
    let outOf = target == "en" || directions["en-\(target)"] != nil
    return into && outOf
  }

  /// The packs a pair needs: one per language other than English, as on
  /// the desktop, each with both its directions.
  public static func packs(_ source: String, _ target: String) -> [String] {
    [source, target].filter { $0 != "en" }
  }

  /// A pack's directions in the manifest ("de-en", "en-de"; "nn-en" alone).
  public func directions(ofPack language: String) -> [String] {
    ["\(language)-en", "en-\(language)"].filter { directions[$0] != nil }
  }
}

/// Packs on the device: `<root>/<language>/<from>-<to>/` holding the
/// direction's files (the folder names lowercased, as the engine looks for
/// them), each pack moved into place whole once its files are checked.
public final class BergamotPacks: @unchecked Sendable {
  /// Downloads `url` to a temporary file, reporting the bytes received so
  /// far.
  public typealias Fetch = @Sendable (URL, @escaping @Sendable (Int64) -> Void) async throws -> URL
  /// Decompresses zstd `from` into `to` (BergamotKit's
  /// `bk_zstd_decompress_file`).
  public typealias Decompress = @Sendable (URL, URL) -> Bool

  public enum Failure: Error, Equatable {
    case notInManifest(String)
    case download(String)
    case mismatch(String)
    case decompress(String)
    case disk
  }

  public let root: URL
  public let manifest: TranslationManifest
  private let fetch: Fetch
  private let decompress: Decompress
  private let lock = NSLock()
  private var running: [String: Task<Void, Error>] = [:]

  public init(root: URL, manifest: TranslationManifest, fetch: @escaping Fetch, decompress: @escaping Decompress) {
    self.root = root
    self.manifest = manifest
    self.fetch = fetch
    self.decompress = decompress
  }

  public func folder(ofPack language: String) -> URL {
    root.appending(path: language.lowercased(), directoryHint: .isDirectory)
  }

  /// Installed packs: a folder with its directions' models.
  public var installed: [String] {
    let names = (try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? []
    return names.filter { name in
      guard !name.hasPrefix("."), let language = pack(named: name) else { return false }
      let directions = manifest.directions(ofPack: language)
      return !directions.isEmpty && directions.allSatisfy {
        FileManager.default.fileExists(atPath: folder(ofPack: language).appending(path: $0.lowercased()).appending(path: "model.bin").path)
      }
    }.compactMap(pack(named:)).sorted()
  }

  /// The manifest's language for a folder name (folders are lowercased).
  private func pack(named folder: String) -> String? {
    manifest.languages.first { $0.lowercased() == folder }
  }

  public func isInstalled(_ language: String) -> Bool { installed.contains(language) }

  /// The packs a pair still needs.
  public func missing(_ source: String, _ target: String) -> [String] {
    let have = Set(installed)
    return TranslationManifest.packs(source, target).filter { !have.contains($0) }
  }

  /// The bytes the packs download (compressed).
  public func downloadSize(_ languages: [String]) -> Int64 {
    languages.flatMap { manifest.directions(ofPack: $0) }
      .compactMap { manifest.directions[$0] }
      .flatMap(\.files).filter { TranslationManifest.fileName($0.type) != nil }
      .reduce(0) { $0 + $1.size }
  }

  /// The engine's packages: (language1, language2, folder) per pack, the
  /// folder holding both directions.
  public var packages: [(String, String, String)] {
    installed.map { ($0.lowercased(), "en", folder(ofPack: $0).path) }
  }

  /// Downloads, checks and installs `languages`' packs, one after the
  /// other, reporting (received, total) bytes across them all. A pack
  /// another call is installing is waited for.
  public func install(_ languages: [String], progress: @escaping @Sendable (Int64, Int64) -> Void) async throws {
    let total = downloadSize(languages)
    var done: Int64 = 0
    for language in languages where !isInstalled(language) {
      let base = done
      let task: Task<Void, Error> = lock.withLock {
        if let task = running[language] { return task }
        let task = Task { try await self.installPack(language) { progress(base + $0, total) } }
        running[language] = task
        return task
      }
      defer { lock.withLock { running[language] = nil } }
      try await task.value
      done += downloadSize([language])
      progress(done, total)
    }
  }

  private func installPack(_ language: String, progress: @escaping @Sendable (Int64) -> Void) async throws {
    let directions = manifest.directions(ofPack: language)
    guard !directions.isEmpty else { throw Failure.notInManifest(language) }
    let fm = FileManager.default
    let staging = root.appending(path: ".staging-\(language.lowercased())", directoryHint: .isDirectory)
    try? fm.removeItem(at: staging)
    do { try fm.createDirectory(at: staging, withIntermediateDirectories: true) } catch { throw Failure.disk }
    defer { try? fm.removeItem(at: staging) }
    var done: Int64 = 0
    for direction in directions {
      let folder = staging.appending(path: direction.lowercased(), directoryHint: .isDirectory)
      do { try fm.createDirectory(at: folder, withIntermediateDirectories: true) } catch { throw Failure.disk }
      for file in manifest.directions[direction]?.files ?? [] {
        guard let name = TranslationManifest.fileName(file.type) else { continue }
        guard let url = URL(string: manifest.attachments + file.location) else { throw Failure.download(file.location) }
        let base = done
        let download: URL
        do {
          download = try await fetch(url) { progress(base + min($0, file.size)) }
        } catch {
          throw Failure.download(file.location)
        }
        defer { try? fm.removeItem(at: download) }
        guard Self.size(download) == file.size, Self.sha256(download) == file.hash else {
          throw Failure.mismatch(file.location)
        }
        let destination = folder.appending(path: name)
        guard decompress(download, destination) else { throw Failure.decompress(file.location) }
        guard Self.size(destination) == file.decompressedSize, Self.sha256(destination) == file.decompressedHash else {
          throw Failure.mismatch(name)
        }
        done += file.size
        progress(done)
      }
    }
    let destination = folder(ofPack: language)
    try? fm.removeItem(at: destination)
    do { try fm.moveItem(at: staging, to: destination) } catch { throw Failure.disk }
  }

  public func uninstall(_ language: String) {
    try? FileManager.default.removeItem(at: folder(ofPack: language))
  }

  static func size(_ url: URL) -> Int64? {
    (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? NSNumber)?.int64Value
  }

  /// SHA-256 of a file, lowercase hex, read a megabyte at a time.
  static func sha256(_ url: URL) -> String? {
    guard let handle = try? FileHandle(forReadingFrom: url) else { return nil }
    defer { try? handle.close() }
    var hasher = SHA256()
    while let chunk = try? handle.read(upToCount: 1 << 20), !chunk.isEmpty {
      hasher.update(data: chunk)
    }
    return hasher.finalize().map { String(format: "%02x", $0) }.joined()
  }
}
