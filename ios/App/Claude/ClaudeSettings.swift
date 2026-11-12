// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import Foundation
import Observation

/// Settings → Claude (docs/design/claude.md, "iOS"): the API key, kept in
/// this device's Keychain only (not shared with the extensions, never in
/// iCloud), and the rest in UserDefaults: the endpoint, its auth style, the
/// model and the whole-browser permission. No key ships with Compass.
@MainActor
@Observable
final class ClaudeSettings {
  static let shared = ClaudeSettings()

  private static let keyAccount = "claude-api-key"
  private let defaults = UserDefaults.standard

  /// "sk-ant-…a1b2" for a saved key; nil without one.
  private(set) var keyHint: String?
  var hasKey: Bool { keyHint != nil }

  /// As typed; empty is the Claude API (`ClaudeEndpoint.parseBaseURL`).
  var baseURLText: String { didSet { defaults.set(baseURLText, forKey: "ClaudeBaseURL") } }
  var authStyle: ClaudeAuthStyle { didSet { defaults.set(authStyle.rawValue, forKey: "ClaudeAuthStyle") } }
  var model: String { didSet { defaults.set(model, forKey: "ClaudeModel") } }
  /// "Let Claude use the whole browser" (off by default; the tab tools come
  /// with I4).
  var browserScope: Bool { didSet { defaults.set(browserScope, forKey: "ClaudeBrowserScope") } }
  /// "Accept the Risks of Subscription Tokens": a token from `claude
  /// setup-token` may be the key (ClaudeEndpoint.isOAuthToken).
  var acceptTokenRisks: Bool {
    didSet {
      defaults.set(acceptTokenRisks, forKey: "ClaudeAcceptTokenRisks")
      revision += 1
    }
  }
  /// "Use Memory": what Claude saved goes into each new chat (on by
  /// default; docs/design/claude.md, "Memory").
  var memoryEnabled: Bool { didSet { defaults.set(memoryEnabled, forKey: "ClaudeMemoryEnabled") } }
  /// "Let Claude Save Memories": the memory tools are sent.
  var memorySave: Bool { didSet { defaults.set(memorySave, forKey: "ClaudeMemorySave") } }
  /// Settings → Claude → Files: what the file tools may do (off by
  /// default; docs/design/claude.md, "Files").
  var fileAccess: ClaudeFileAccess { didSet { defaults.set(fileAccess.rawValue, forKey: "ClaudeFilesAccess") } }
  /// The picked folder's name; nil without one (Save Files then uses the
  /// app's Downloads folder).
  private(set) var folderName: String?
  /// The picked folder, as a security-scoped bookmark.
  private var folderBookmark: Data? {
    didSet { defaults.set(folderBookmark, forKey: "ClaudeFilesFolder") }
  }
  /// What "Test connection" listed, else the known models.
  var models: [ClaudeModel] = ClaudeModel.known

  /// Bumped when the key or endpoint changes: replies in flight stop.
  private(set) var revision = 0

  private init() {
    baseURLText = defaults.string(forKey: "ClaudeBaseURL") ?? ""
    authStyle = defaults.string(forKey: "ClaudeAuthStyle").flatMap(ClaudeAuthStyle.init(rawValue:)) ?? .apiKey
    model = defaults.string(forKey: "ClaudeModel") ?? ClaudeEndpoint.defaultModel
    browserScope = defaults.bool(forKey: "ClaudeBrowserScope")
    acceptTokenRisks = defaults.bool(forKey: "ClaudeAcceptTokenRisks")
    memoryEnabled = defaults.object(forKey: "ClaudeMemoryEnabled") as? Bool ?? true
    memorySave = defaults.object(forKey: "ClaudeMemorySave") as? Bool ?? true
    keyHint = Self.storedKey.map(ClaudeEndpoint.keyHint)
    fileAccess = ClaudeFileAccess(rawValue: defaults.integer(forKey: "ClaudeFilesAccess")) ?? .off
    folderBookmark = defaults.data(forKey: "ClaudeFilesFolder")
    folderName = folderBookmark.flatMap(Self.resolve)?.url.lastPathComponent
  }

  // MARK: Files

  /// Save Files' folder without a picked one: Compass's Downloads (in
  /// Files, On My iPhone › Compass › Downloads), as the desktop's.
  static var documents: URL { Downloads.folderURL }
  static let documentsName = "Downloads"

  /// Keeps a folder from the document picker (its access already started
  /// by the picker): false if it can't be kept.
  @discardableResult
  func setFolder(_ url: URL) -> Bool {
    let accessing = url.startAccessingSecurityScopedResource()
    defer { if accessing { url.stopAccessingSecurityScopedResource() } }
    guard let bookmark = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)
    else { return false }
    folderBookmark = bookmark
    folderName = url.lastPathComponent
    return true
  }

  func clearFolder() {
    folderBookmark = nil
    folderName = nil
  }

  private static func resolve(_ bookmark: Data) -> (url: URL, stale: Bool)? {
    var stale = false
    guard let url = try? URL(resolvingBookmarkData: bookmark, options: [], relativeTo: nil, bookmarkDataIsStale: &stale)
    else { return nil }
    return (url, stale)
  }

  /// A folder the file tools work in, opened until `close()`.
  struct OpenFolder {
    let url: URL
    let name: String
    let path: String
    fileprivate let scoped: Bool
    func close() { if scoped { url.stopAccessingSecurityScopedResource() } }
  }

  /// The folder for `access`, opened: the picked one, else (Save Files)
  /// the app's Documents. nil when there's none to use; a bookmark that no
  /// longer resolves is dropped, so settings ask for a folder again.
  func openFolder(for access: ClaudeFileAccess) -> OpenFolder? {
    guard access != .off else { return nil }
    if let bookmark = folderBookmark {
      guard let resolved = Self.resolve(bookmark) else {
        clearFolder()
        return access == .save ? documentsFolder : nil
      }
      let (url, stale) = resolved
      let scoped = url.startAccessingSecurityScopedResource()
      if stale, let renewed = try? url.bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil) {
        folderBookmark = renewed
      }
      folderName = url.lastPathComponent
      return OpenFolder(url: url, name: url.lastPathComponent, path: url.path, scoped: scoped)
    }
    return access == .save ? documentsFolder : nil
  }

  private var documentsFolder: OpenFolder {
    OpenFolder(url: Self.documents, name: Self.documentsName, path: Self.documents.path, scoped: false)
  }

  /// The context line for the next message (the desktop's).
  var filesContextLine: String {
    switch fileAccess {
    case .off: return ClaudeFiles.contextLine(.off, folderName: "", folderPath: "")
    case .save: return ClaudeFiles.contextLine(.save, folderName: folderName ?? Self.documentsName, folderPath: "")
    case .folder:
      guard let folder = openFolder(for: .folder) else { return ClaudeFiles.contextLine(.off, folderName: "", folderPath: "") }
      folder.close()
      // The folder's name: its path on iOS means nothing to the user.
      return ClaudeFiles.contextLine(.folder, folderName: folder.name, folderPath: "“\(folder.name)”")
    }
  }

  private static var storedKey: String? {
    Keychain.data(keyAccount).flatMap { String(data: $0, encoding: .utf8) }.flatMap { $0.isEmpty ? nil : $0 }
  }

  func setKey(_ key: String) {
    let key = key.trimmingCharacters(in: .whitespacesAndNewlines)
    Keychain.set(key.isEmpty ? nil : Data(key.utf8), Self.keyAccount)
    keyHint = key.isEmpty ? nil : ClaudeEndpoint.keyHint(key)
    revision += 1
  }

  func removeKey() { setKey("") }

  /// The typed endpoint URL is usable (or empty).
  var baseURLIsValid: Bool { ClaudeEndpoint.parseBaseURL(baseURLText) != nil }

  /// Where requests go, with the key; nil without a key or with a bad URL.
  var endpoint: ClaudeEndpoint? {
    guard let key = Self.storedKey, let base = ClaudeEndpoint.parseBaseURL(baseURLText) else { return nil }
    return ClaudeEndpoint(baseURL: base, authStyle: authStyle, apiKey: key, allowOAuthToken: acceptTokenRisks)
  }

  func endpointChanged() { revision += 1 }

  /// The picker's models: the listed (or known) ones, plus the saved one if
  /// it isn't among them (a gateway's own id).
  var pickerModels: [ClaudeModel] {
    models.contains { $0.id == model } ? models : [ClaudeModel(id: model, displayName: model)] + models
  }

  // MARK: Shared files (common/claude/, bundled)

  static let systemPrompt: String = bundled("system_prompt", "md") ?? ""
  static let pageAgent: String = bundled("page_agent", "js") ?? ""
  /// The tool definitions sent to the API (each entry's `definition`;
  /// its `scope` stays here, ClaudeTools.scopes).
  /// Entries for other platforms (`"platforms": ["desktop"]`) are left
  /// out; an entry without `platforms` is for every platform.
  static let tools: [JSONValue] = toolEntries.filter { $0["scope"]?.string != "memory" }.compactMap { $0["definition"] }
  /// The memory tools (remember, update_memory, forget), sent while Claude
  /// may save memories.
  static let memoryTools: [JSONValue] = toolEntries.filter { $0["scope"]?.string == "memory" }
    .compactMap { $0["definition"] }
  private static let toolEntries: [JSONValue] =
    bundled("tools", "json").flatMap { JSONValue.parse($0)?["tools"]?.array }?.filter(isForIOS) ?? []

  nonisolated static func isForIOS(_ entry: JSONValue) -> Bool {
    guard let platforms = entry["platforms"]?.array else { return true }
    return platforms.contains("ios")
  }

  private static func bundled(_ name: String, _ ext: String) -> String? {
    Bundle.main.url(forResource: name, withExtension: ext).flatMap { try? String(contentsOf: $0, encoding: .utf8) }
  }
}
