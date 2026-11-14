// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Claude's small background requests for the sidebar (docs/design/
/// ai-sidebar.md, "Shared core"; the desktop's browser/claude/
/// claude_quick.*): names for link folders and Tidy Today now, short titles
/// later. One system prompt and one user message, no tools, at most
/// `maxTokens`, through the chat's endpoint, key, auth style and cache
/// level, on the feature's own model; one JSON object back within
/// `timeout`. Never shows UI. Background requests are limited to
/// `maxBackgroundPerHour` an hour, and an error stops them for
/// `firstBackOff`, a second one in a row for `laterBackOff`.
///
/// Each feature has its own switch (off by default) and model (Settings →
/// Claude): nothing is sent while the feature's switch is off. Incognito
/// never asks: callers leave its entries out.
@MainActor
public final class ClaudeQuick {
  /// Enough for Tidy Today's reply, which lists its folders' entries.
  public nonisolated static let maxTokens = 400
  public nonisolated static let maxBackgroundPerHour = 30
  public nonisolated static let firstBackOff: TimeInterval = 60
  public nonisolated static let laterBackOff: TimeInterval = 10 * 60
  /// The model a feature asks when its setting names none.
  public nonisolated static let defaultModel = "claude-haiku-5-5"
  /// A reply this long isn't a name, or anything else these requests ask for.
  nonisolated static let maxReplyText = 8 * 1024

  /// One request.
  public struct Request: Sendable {
    /// The feature's own switch (its UserDefaults key, the desktop's pref
    /// name, e.g. `ClaudeQuick.nameLinkFoldersKey`): nothing is sent while
    /// it's off.
    public var feature: String
    /// The model the feature's setting names; empty for `defaultModel`.
    public var model: String
    public var system: String
    public var user: String
    /// Capped at `maxTokens`.
    public var maxTokens: Int
    /// An automatic request, which counts against the hourly limit and
    /// waits out the back-off. One the user asked for (Suggest a Name,
    /// Tidy) doesn't.
    public var background: Bool

    public init(
      feature: String, model: String = "", system: String, user: String, maxTokens: Int = 100,
      background: Bool = true
    ) {
      self.feature = feature
      self.model = model
      self.system = system
      self.user = user
      self.maxTokens = maxTokens
      self.background = background
    }
  }

  public enum Failure: Error, Equatable, Sendable {
    /// The feature's switch is off.
    case off
    /// No key saved (or a subscription token whose risks aren't accepted).
    case notSetUp
    /// The hour's background requests are used up.
    case rateLimited
    /// Waiting after an error.
    case backingOff
    /// The endpoint answered with an error, or couldn't be reached.
    case failed
    /// No answer in `timeout`.
    case timeout
    /// Claude declined (stop_reason "refusal").
    case refused
    /// The answer isn't one JSON object.
    case badReply
  }

  /// Sends a request and gives back the body and the HTTP status.
  public typealias Transport = @Sendable (URLRequest) async throws -> (Data, Int)

  /// Where requests go, with the key; nil while Claude isn't set up.
  let endpoint: @MainActor () -> ClaudeEndpoint?
  /// Whether a feature's switch is on.
  let isOn: @MainActor (String) -> Bool
  /// The chat's cache level for an endpoint (`CacheLevelMemory`).
  let cacheLevel: @MainActor (ClaudeEndpoint) -> ChatRequest.CacheLevel
  let transport: Transport
  /// The answer, or giving up, within this.
  public var timeout: TimeInterval = 15
  public var now: () -> Date = Date.init

  /// When the last hour's background requests went.
  private var recent: [Date] = []
  /// Errors in a row, and until when background requests wait.
  private var failures = 0
  private var backingOffUntil = Date.distantPast

  public init(
    endpoint: @escaping @MainActor () -> ClaudeEndpoint?, isOn: @escaping @MainActor (String) -> Bool,
    cacheLevel: @escaping @MainActor (ClaudeEndpoint) -> ChatRequest.CacheLevel = { _ in .normal },
    transport: @escaping Transport = ClaudeQuick.urlSession
  ) {
    self.endpoint = endpoint
    self.isOn = isOn
    self.cacheLevel = cacheLevel
    self.transport = transport
  }

  /// URLSession, without cookies or a cache.
  public nonisolated static let urlSession: Transport = { request in
    let (data, response) = try await session.data(for: request)
    return (data, (response as? HTTPURLResponse)?.statusCode ?? 0)
  }

  private nonisolated static let session: URLSession = {
    let configuration = URLSessionConfiguration.ephemeral
    configuration.httpCookieStorage = nil
    configuration.urlCache = nil
    return URLSession(configuration: configuration)
  }()

  /// Whether Claude is set up: a key is saved (the Claude API's or a
  /// gateway's), and usable.
  public var isSetUp: Bool {
    guard let endpoint = endpoint() else { return false }
    return !endpoint.needsTokenRisksAccepted
  }

  /// `isSetUp`, and the feature's switch is on.
  public func isEnabled(_ feature: String) -> Bool { isSetUp && !feature.isEmpty && isOn(feature) }

  /// Background requests sent in the last hour.
  public var recentBackgroundRequests: Int {
    prune(now())
    return recent.count
  }

  /// Sends `request`: the reply's JSON object, or why not.
  public func ask(_ request: Request) async -> Result<[String: JSONValue], Failure> {
    guard let endpoint = endpoint(), !endpoint.needsTokenRisksAccepted else { return .failure(.notSetUp) }
    guard isEnabled(request.feature) else { return .failure(.off) }
    let start = now()
    if request.background {
      if start < backingOffUntil { return .failure(.backingOff) }
      prune(start)
      if recent.count >= Self.maxBackgroundPerHour { return .failure(.rateLimited) }
      recent.append(start)
    }
    let body = Self.body(
      request, model: request.model.isEmpty ? Self.defaultModel : request.model,
      cacheLevel: cacheLevel(endpoint), oauthToken: endpoint.usesOAuthToken)
    var urlRequest = endpoint.request("v1/messages", body: body.encoded())
    urlRequest.timeoutInterval = timeout
    let result = await Self.send(urlRequest, transport: transport, timeout: timeout)
    switch result {
    case .success:
      failures = 0
    case .failure(.failed), .failure(.timeout):
      // The endpoint is down, or refusing: wait before asking again.
      failures += 1
      backingOffUntil = now().addingTimeInterval(failures == 1 ? Self.firstBackOff : Self.laterBackOff)
    case .failure:
      break
    }
    return result
  }

  private func prune(_ now: Date) {
    recent.removeAll { now.timeIntervalSince($0) >= 60 * 60 }
  }

  /// One request through `transport`, given up after `timeout`.
  nonisolated static func send(
    _ request: URLRequest, transport: @escaping Transport, timeout: TimeInterval
  ) async -> Result<[String: JSONValue], Failure> {
    await withTaskGroup(of: Result<[String: JSONValue], Failure>.self) { group in
      group.addTask {
        do {
          let (data, status) = try await transport(request)
          return parseResponse(data, status: status)
        } catch {
          return .failure(Task.isCancelled ? .timeout : .failed)
        }
      }
      group.addTask {
        try? await Task.sleep(for: .seconds(timeout))
        return .failure(.timeout)
      }
      let first = await group.next() ?? .failure(.timeout)
      group.cancelAll()
      return first
    }
  }

  /// A Messages API response as this client takes it: its text as one JSON
  /// object.
  nonisolated static func parseResponse(_ data: Data, status: Int) -> Result<[String: JSONValue], Failure> {
    guard status == 200, let message = JSONValue.parse(data), message["type"]?.string != "error" else {
      return .failure(.failed)
    }
    if message["stop_reason"]?.string == "refusal" { return .failure(.refused) }
    let text = (message["content"]?.array ?? [])
      .filter { $0["type"]?.string == "text" }
      .compactMap { $0["text"]?.string }
      .joined()
    guard text.utf8.count <= maxReplyText, let reply = parseReply(text) else { return .failure(.badReply) }
    return .success(reply)
  }

  /// The Messages API body: the model, no tools, no thinking, not streamed;
  /// the system prompt with the cache breakpoints `cacheLevel` allows (and
  /// Claude Code's line first for a subscription token, as the chat's).
  /// The desktop's BuildQuickRequest().
  public nonisolated static func body(
    _ request: Request, model: String, cacheLevel: ChatRequest.CacheLevel, oauthToken: Bool
  ) -> JSONValue {
    let ephemeral: JSONValue = ["type": "ephemeral"]
    var system: [JSONValue] = []
    if oauthToken { system.append(["type": "text", "text": .string(ChatRequest.oauthSystemLine)]) }
    var prompt: [String: JSONValue] = ["type": "text", "text": .string(request.system)]
    if cacheLevel == .normal { prompt["cache_control"] = ephemeral }
    system.append(.object(prompt))
    var body: [String: JSONValue] = [
      "model": .string(model),
      "max_tokens": .number(Double(min(max(request.maxTokens, 1), maxTokens))),
      "system": .array(system),
      "messages": [["role": "user", "content": [["type": "text", "text": .string(request.user)]]]],
    ]
    if cacheLevel != .none { body["cache_control"] = ephemeral }
    return .object(body)
  }

  /// A reply's text as the one JSON object it must be (whitespace around it
  /// only). Nil for anything else.
  public nonisolated static func parseReply(_ text: String) -> [String: JSONValue]? {
    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard trimmed.hasPrefix("{") else { return nil }
    return JSONValue.parse(trimmed)?.object
  }
}

// MARK: Settings

extension ClaudeQuick {
  /// "Name Link Folders": the switch and its model, as UserDefaults keys
  /// named as the desktop's prefs (browser/claude/claude_prefs.h).
  public nonisolated static let nameLinkFoldersKey = "compass.claude.name_link_folders"
  public nonisolated static let nameLinkFoldersModelKey = "compass.claude.name_link_folders_model"
  /// "Tidy Today": Today's header offers Tidy, which asks Claude to group
  /// Today's loose entries into named folders; and the model it asks.
  public nonisolated static let tidyTodayKey = "compass.claude.tidy_today"
  public nonisolated static let tidyTodayModelKey = "compass.claude.tidy_today_model"
}

// MARK: Prompts and names

/// common/claude/sidebar_prompts.md: one `## <id>` section per feature.
public enum SidebarPrompts {
  /// The section `id` of `prompts` (the file's text), trimmed, with
  /// `{locale}` replaced by `locale`. Empty if there's no such section. The
  /// desktop's SidebarPromptFrom().
  public static func section(_ id: String, in prompts: String, locale: String) -> String {
    let heading = "## " + id
    var lines: [Substring] = []
    var inSection = false
    for line in prompts.split(separator: "\n", omittingEmptySubsequences: false) {
      var trimmed = line
      while let last = trimmed.last, last == " " || last == "\t" || last == "\r" { trimmed.removeLast() }
      if trimmed.hasPrefix("## ") {
        if inSection { break }
        inSection = trimmed == heading
        continue
      }
      if inSection { lines.append(trimmed) }
    }
    return lines.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
      .replacingOccurrences(of: "{locale}", with: locale)
  }
}

/// A name Claude gives (a folder's): at most this many characters and words.
public let maxAiNameLength = 32
public let maxAiNameWords = 3

/// `name` as the sidebar may show it: trimmed (also of quotes around it and
/// a full stop at the end), its spaces collapsed, 1 to `maxAiNameWords`
/// words of at most `maxAiNameLength` characters, with no emoji, control
/// characters, line breaks or markup. Nil if it can't be one. The desktop's
/// SanitizeAiName().
public func sanitizeAiName(_ name: String) -> String? {
  // Line breaks never belong in a name; anything else around it goes.
  if name.unicodeScalars.contains(where: { $0 == "\r" || $0 == "\n" }) { return nil }
  var text = name.trimmingCharacters(in: .whitespaces)
  let quotes: Set<Unicode.Scalar> = ["\"", "'", "`", "\u{201C}", "\u{201D}", "\u{2018}", "\u{2019}", "\u{00AB}", "\u{00BB}"]
  // Quotes around it, and a full stop after it.
  while text.unicodeScalars.count >= 2, let first = text.unicodeScalars.first, let last = text.unicodeScalars.last,
    quotes.contains(first), quotes.contains(last)
  {
    text = String(Substring(text.unicodeScalars.dropFirst().dropLast()))
      .trimmingCharacters(in: .whitespaces)
  }
  while let last = text.unicodeScalars.last, last == "." || last == "\u{3002}" {
    text = String(Substring(text.unicodeScalars.dropLast()))
  }
  // Spaces collapsed.
  text = text.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
  guard !text.isEmpty else { return nil }
  var hasAlnum = false
  for scalar in text.unicodeScalars {
    if isRefusedInName(scalar) { return nil }
    hasAlnum = hasAlnum || isLetterOrDigit(scalar)
  }
  guard hasAlnum, text.unicodeScalars.count <= maxAiNameLength else { return nil }
  let words = text.split(separator: " ").count
  guard words >= 1, words <= maxAiNameWords else { return nil }
  return text
}

// MARK: Tidy Today's reply

/// Tidy Today (docs/design/ai-sidebar.md): the most folders one reply makes,
/// and the fewest entries a folder takes.
public let maxTidyFolders = 6
public let minTidyFolderEntries = 2

/// A folder Tidy Today's reply asks for: its name (as `sanitizeAiName()` left
/// it) and its entries' ids, in the reply's order.
public struct TidyFolder: Sendable, Equatable {
  public var name: String
  public var entries: [String]

  public init(name: String, entries: [String]) {
    self.name = name
    self.entries = entries
  }
}

/// The folders of Tidy Today's reply, `{"folders": [{"name": "…", "entries":
/// ["<id>", …]}, …]}`, that may be made: ids not in `ids`, or in this folder
/// or an earlier one already, are dropped; then a folder whose name
/// `sanitizeAiName()` drops, or with fewer than `minTidyFolderEntries`
/// entries left, is dropped. At most the first `maxTidyFolders` that pass.
/// Empty for a reply of another shape. The desktop's ParseTidyReply().
public func parseTidyReply(_ reply: [String: JSONValue], ids: [String]) -> [TidyFolder] {
  guard let list = reply["folders"]?.array else { return [] }
  let known = Set(ids)
  var taken: Set<String> = []
  var folders: [TidyFolder] = []
  for item in list {
    if folders.count == maxTidyFolders { break }
    guard let name = item["name"]?.string, let entries = item["entries"]?.array,
      let sanitized = sanitizeAiName(name)
    else { continue }
    var folder = TidyFolder(name: sanitized, entries: [])
    var inFolder: Set<String> = []
    for entry in entries {
      // Ids are strings; a whole number for one is taken too.
      let id: String
      if let string = entry.string {
        id = string
      } else if let number = entry.number, number == number.rounded(), abs(number) < 1e15 {
        id = String(Int(number))
      } else {
        continue
      }
      guard known.contains(id), !taken.contains(id), inFolder.insert(id).inserted else { continue }
      folder.entries.append(id)
    }
    guard folder.entries.count >= minTidyFolderEntries else { continue }
    taken.formUnion(folder.entries)
    folders.append(folder)
  }
  return folders
}

/// What a name may not hold: emoji, control and format characters (which
/// emoji sequences and bidi tricks use), private use, line breaks, and the
/// leftovers of markup or JSON.
func isRefusedInName(_ c: Unicode.Scalar) -> Bool {
  if "<>{}[]\"`\\|".unicodeScalars.contains(c) { return true }
  let properties = c.properties
  // Emoji: ASCII digits, # and * are emoji too (as keycap bases), not refused.
  if properties.isEmojiPresentation || properties.isEmojiModifier || (properties.isEmoji && !c.isASCII) {
    return true
  }
  if (0x1F1E6...0x1F1FF).contains(c.value) { return true }  // regional indicators
  switch properties.generalCategory {
  case .control, .format, .privateUse, .surrogate, .unassigned, .lineSeparator, .paragraphSeparator:
    return true
  default:
    return false
  }
}

private func isLetterOrDigit(_ c: Unicode.Scalar) -> Bool {
  switch c.properties.generalCategory {
  case .uppercaseLetter, .lowercaseLetter, .titlecaseLetter, .modifierLetter, .otherLetter, .decimalNumber:
    return true
  default:
    return false
  }
}
