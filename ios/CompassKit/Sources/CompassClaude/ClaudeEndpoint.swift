// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// How the key goes with requests: the Claude API's `x-api-key`, or
/// `Authorization: Bearer` for gateways that want it.
public enum ClaudeAuthStyle: String, CaseIterable, Sendable {
  case apiKey = "x-api-key"
  case bearer = "bearer"
}

/// A model the picker offers.
public struct ClaudeModel: Equatable, Sendable, Identifiable {
  public var id: String
  public var displayName: String

  public init(id: String, displayName: String) {
    self.id = id
    self.displayName = displayName
  }

  /// Offered until "Test connection" lists the ones the key can use; the
  /// desktop's list (claude_page.ts).
  public static let known: [ClaudeModel] = [
    ClaudeModel(id: "claude-opus-5-5", displayName: "Claude Opus 5.5"),
    ClaudeModel(id: "claude-sonnet-5-5", displayName: "Claude Sonnet 5.5"),
    ClaudeModel(id: "claude-haiku-5-5", displayName: "Claude Haiku 5.5"),
    ClaudeModel(id: "claude-fable-5-1", displayName: "Claude Fable 5.1"),
  ]
}

/// Where requests go and how they're signed: the desktop's Endpoint
/// (browser/claude/claude_api.cc).
public struct ClaudeEndpoint: Sendable {
  public static let defaultBaseURL = URL(string: "https://api.anthropic.com")!
  public static let apiVersion = "2026-10-21"
  public static let defaultModel = "claude-opus-5-5"

  public var baseURL: URL
  public var authStyle: ClaudeAuthStyle
  public var apiKey: String

  public init(baseURL: URL = Self.defaultBaseURL, authStyle: ClaudeAuthStyle = .apiKey, apiKey: String) {
    self.baseURL = baseURL
    self.authStyle = authStyle
    self.apiKey = apiKey
  }

  /// The Claude API itself, not a custom endpoint (server-side fallbacks
  /// are the Claude API's).
  public var isClaudeAPI: Bool { baseURL == Self.defaultBaseURL }

  /// A typed base URL, checked as the desktop's `ParseBaseUrl()`: empty is
  /// the Claude API; https, or http only to this device (local gateways); no
  /// query, fragment or credentials, since the key goes with every request.
  /// A path is kept, for gateways that serve the API under one.
  public static func parseBaseURL(_ text: String) -> URL? {
    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    if trimmed.isEmpty { return defaultBaseURL }
    guard var components = URLComponents(string: trimmed), let scheme = components.scheme?.lowercased(),
      let host = components.host, !host.isEmpty,
      components.query == nil, components.fragment == nil, components.user == nil, components.password == nil
    else { return nil }
    let local = ["localhost", "127.0.0.1", "::1", "[::1]"].contains(host.lowercased())
    guard scheme == "https" || (scheme == "http" && local) else { return nil }
    var path = components.path
    while path.count > 1, path.hasSuffix("/") { path.removeLast() }
    if path == "/" { path = "" }
    components.path = path
    return components.url
  }

  /// `path` under the base URL ("v1/messages").
  public func url(_ path: String) -> URL {
    var base = baseURL.absoluteString
    if !base.hasSuffix("/") { base += "/" }
    return URL(string: base + path)!
  }

  /// A request with the version and key headers, and any betas.
  public func request(_ path: String, body: Data? = nil, betas: [String] = []) -> URLRequest {
    var request = URLRequest(url: url(path))
    request.httpMethod = body == nil ? "GET" : "POST"
    request.setValue(Self.apiVersion, forHTTPHeaderField: "anthropic-version")
    switch authStyle {
    case .apiKey: request.setValue(apiKey, forHTTPHeaderField: "x-api-key")
    case .bearer: request.setValue("Bearer \(apiKey)", forHTTPHeaderField: "Authorization")
    }
    if !betas.isEmpty { request.setValue(betas.joined(separator: ","), forHTTPHeaderField: "anthropic-beta") }
    if let body {
      request.httpBody = body
      request.setValue("application/json", forHTTPHeaderField: "content-type")
    }
    return request
  }

  /// What the user reads for a failed request: the API's own message, else
  /// one per status. 0 means the server couldn't be reached.
  public static func describeError(status: Int, body: Data) -> String {
    if status == 0 { return "Couldn't reach the server. Check the connection and the endpoint URL." }
    if let message = JSONValue.parse(body)?["error"]?["message"]?.string, !message.isEmpty { return message }
    switch status {
    case 401: return "The key was refused (401). Check it, and the auth style."
    case 403: return "The key isn't allowed to do this (403)."
    case 404: return "Nothing answers at that URL (404). Check the endpoint URL."
    case 429: return "Too many requests (429). Wait a moment and try again."
    case 529: return "Claude is overloaded (529). Try again in a moment."
    default: return "The server answered with HTTP \(status)."
    }
  }

  /// "sk-ant-…a1b2": what settings show of a saved key.
  public static func keyHint(_ key: String) -> String {
    let key = key.trimmingCharacters(in: .whitespacesAndNewlines)
    guard key.count >= 12 else { return "…" }
    return "\(key.prefix(7))…\(key.suffix(4))"
  }

  /// `GET /v1/models`' list; nil if it isn't one.
  public static func parseModels(_ data: Data) -> [ClaudeModel]? {
    guard let list = JSONValue.parse(data)?["data"]?.array else { return nil }
    return list.compactMap { item in
      guard let id = item["id"]?.string, !id.isEmpty else { return nil }
      let name = item["display_name"]?.string ?? ""
      return ClaudeModel(id: id, displayName: name.isEmpty ? id : name)
    }
  }
}
