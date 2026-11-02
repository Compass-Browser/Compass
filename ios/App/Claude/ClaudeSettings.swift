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
  /// What "Test connection" listed, else the known models.
  var models: [ClaudeModel] = ClaudeModel.known

  /// Bumped when the key or endpoint changes: replies in flight stop.
  private(set) var revision = 0

  private init() {
    baseURLText = defaults.string(forKey: "ClaudeBaseURL") ?? ""
    authStyle = defaults.string(forKey: "ClaudeAuthStyle").flatMap(ClaudeAuthStyle.init(rawValue:)) ?? .apiKey
    model = defaults.string(forKey: "ClaudeModel") ?? ClaudeEndpoint.defaultModel
    browserScope = defaults.bool(forKey: "ClaudeBrowserScope")
    keyHint = Self.storedKey.map(ClaudeEndpoint.keyHint)
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
    return ClaudeEndpoint(baseURL: base, authStyle: authStyle, apiKey: key)
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
  static let tools: [JSONValue] = bundled("tools", "json").flatMap { JSONValue.parse($0)?.array } ?? []

  private static func bundled(_ name: String, _ ext: String) -> String? {
    Bundle.main.url(forResource: name, withExtension: ext).flatMap { try? String(contentsOf: $0, encoding: .utf8) }
  }
}
