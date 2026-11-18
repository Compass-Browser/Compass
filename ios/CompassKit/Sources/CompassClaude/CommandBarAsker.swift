// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Observation

/// Ask in the command bar (docs/design/ai-sidebar.md, "Ask in the command
/// bar"): a line typed in the command bar that reads as a question gets an
/// "Ask Claude" row; asked, a short answer streams in under the field,
/// through the chat's streaming client, on the feature's own model, with no
/// tools and a short-answer prompt. "About this page" adds the shown tab's
/// title and host, never what the page holds.
///
/// Only with "Answer Questions in the Command Bar" on (and Claude set up);
/// never in incognito. The user asks: no hourly limit, no back-off.
@MainActor
@Observable
public final class CommandBarAsker {
  /// The prompts file's section for these requests.
  public nonisolated static let promptID = "command_bar_ask"
  /// Enough for a few lines.
  public nonisolated static let maxTokens = 400
  /// The answer, or giving up (the desktop's kStreamTimeout).
  public nonisolated static let answerTimeout: Duration = .seconds(60)
  /// The most of a question or a page's title that's sent (UTF-8 bytes).
  nonisolated static let maxQuestionLength = 2000
  nonisolated static let maxTitleLength = 200

  public enum State: Equatable, Sendable {
    case idle
    /// The answer is streaming.
    case asking
    case answered
    /// Why there's no (whole) answer.
    case failed(String)
  }

  /// Sends a streamed request: each server-sent event to `onEvent`; nil
  /// when the reply ended properly (or was cancelled), else what went
  /// wrong, for the user. The app's ClaudeClient.stream.
  public typealias Stream = @MainActor @Sendable (
    _ body: JSONValue, _ endpoint: ClaudeEndpoint, _ onEvent: @escaping @MainActor (String, String) -> Void
  ) async -> String?

  /// The question asked, and its answer so far.
  public private(set) var question = ""
  public private(set) var answer = ""
  /// The message the question went as (with "About this page"'s line):
  /// Continue in Chat starts with it.
  public private(set) var message = ""
  public private(set) var state: State = .idle

  let quick: ClaudeQuick
  /// common/claude/sidebar_prompts.md's text.
  let prompts: String
  /// The model "Answer Questions in the Command Bar" asks.
  let model: @MainActor () -> String
  /// The browser's language, a BCP 47 tag ("it-IT").
  let locale: @MainActor () -> String
  let stream: Stream
  /// How long an answer may take (`timeout`; tests shorten it).
  @ObservationIgnored public var timeout: Duration = CommandBarAsker.answerTimeout
  @ObservationIgnored private var task: Task<Void, Never>?
  /// Bumped by each question and `cancel()`: a stream's late events drop.
  @ObservationIgnored private var generation = 0

  public init(
    quick: ClaudeQuick, prompts: String, model: @escaping @MainActor () -> String,
    locale: @escaping @MainActor () -> String = { Locale.preferredLanguages.first ?? "en-US" },
    stream: @escaping Stream
  ) {
    self.quick = quick
    self.prompts = prompts
    self.model = model
    self.locale = locale
    self.stream = stream
  }

  /// Whether the switch is on and Claude set up.
  public var isEnabled: Bool { quick.isEnabled(ClaudeQuick.commandBarAskKey) }

  /// Whether the command bar offers "Ask Claude" for `text`.
  public func offers(_ text: String, incognito: Bool) -> Bool {
    !incognito && isEnabled && Self.isQuestion(text, locale: locale())
  }

  /// Asks `question`, streaming the answer into `answer`. `page` is the
  /// shown tab's title and host, when "About this page" is ticked.
  public func ask(_ question: String, page: (title: String, host: String)?, incognito: Bool) {
    cancel()
    let question = question.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !incognito, !question.isEmpty, isEnabled, let endpoint = quick.endpoint(),
      !endpoint.needsTokenRisksAccepted
    else { return }
    self.question = question
    message = Self.userMessage(question: question, page: page)
    answer = ""
    state = .asking
    let body = Self.body(
      question: question, page: page,
      system: SidebarPrompts.section(Self.promptID, in: prompts, locale: locale()),
      model: model().isEmpty
        ? (endpoint.provider == .anthropic ? ClaudeQuick.defaultAskModel : endpoint.provider.defaultAskModel)
        : model(), cacheLevel: quick.cacheLevel(endpoint),
      oauthToken: endpoint.usesOAuthToken)
    generation += 1
    let current = generation
    let send = self.stream
    let timeout = self.timeout
    let stream = Task { [weak self] in
      let error = await send(body, endpoint) { [weak self] name, data in
        guard let self, self.generation == current else { return }
        // Turned off while it streamed: the answer stops there.
        guard self.isEnabled else {
          self.cancel()
          return
        }
        if let text = Self.textDelta(name, data) { self.answer += text }
      }
      guard let self, self.generation == current, !Task.isCancelled else { return }
      if let error {
        self.state = .failed(error)
      } else if self.answer.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
        self.state = .failed("Claude didn't answer.")
      } else {
        self.state = .answered
      }
    }
    task = stream
    // 60 s at most: then what came is kept, with the error.
    Task { [weak self] in
      try? await Task.sleep(for: timeout)
      guard let self, self.generation == current, self.state == .asking else { return }
      self.generation += 1
      stream.cancel()
      self.task = nil
      self.state = .failed("Claude took too long.")
    }
  }

  /// Stops the answer and forgets it (the field changed, the bar closed).
  public func cancel() {
    generation += 1
    task?.cancel()
    task = nil
    question = ""
    message = ""
    answer = ""
    state = .idle
  }

  // MARK: Pieces

  /// The text a streamed event adds: a `text_delta`'s.
  public nonisolated static func textDelta(_ name: String, _ data: String) -> String? {
    guard name == "content_block_delta", let event = JSONValue.parse(data),
      event["delta"]?["type"]?.string == "text_delta"
    else { return nil }
    return event["delta"]?["text"]?.string
  }

  /// "About this page": the line that names the shown tab, by its title and
  /// host only; empty for none. The desktop's DescribeAskPage().
  public nonisolated static func describePage(title: String, host: String) -> String {
    if title.isEmpty && host.isEmpty { return "" }
    let title = cut(oneLine(title), maxTitleLength)
    var text = "I'm looking at a page"
    if !title.isEmpty { text += " titled \"\(title)\"" }
    if !host.isEmpty { text += " on \(host)" }
    return text + "."
  }

  /// The user message: the page's line when asked about it, then the
  /// question (the desktop's MakeAskRequest()).
  public nonisolated static func userMessage(question: String, page: (title: String, host: String)?) -> String {
    let line = page.map { describePage(title: $0.title, host: $0.host) } ?? ""
    let question = cut(question, maxQuestionLength)
    return line.isEmpty ? question : line + "\n\n" + question
  }

  /// The Messages API body: the model, no tools, no thinking, streamed; the
  /// prompt with the cache breakpoints `cacheLevel` allows (and Claude
  /// Code's line first for a subscription token, as the chat's).
  public nonisolated static func body(
    question: String, page: (title: String, host: String)?, system prompt: String, model: String,
    cacheLevel: ChatRequest.CacheLevel, oauthToken: Bool
  ) -> JSONValue {
    let ephemeral: JSONValue = ["type": "ephemeral"]
    var system: [JSONValue] = []
    if oauthToken { system.append(["type": "text", "text": .string(ChatRequest.oauthSystemLine)]) }
    var block: [String: JSONValue] = ["type": "text", "text": .string(prompt)]
    if cacheLevel == .normal { block["cache_control"] = ephemeral }
    system.append(.object(block))
    var body: [String: JSONValue] = [
      "model": .string(model),
      "max_tokens": .number(Double(maxTokens)),
      "system": .array(system),
      "messages": [
        ["role": "user", "content": [["type": "text", "text": .string(userMessage(question: question, page: page))]]]
      ],
      "stream": true,
    ]
    if cacheLevel != .none { body["cache_control"] = ephemeral }
    return .object(body)
  }

  // MARK: Questions

  /// Words (and pairs of words) a question starts with, by language: the
  /// desktop's LooksLikeQuestion() list. English's always count, another
  /// language's only in that language.
  public nonisolated static let questionWords: [String: [String]] = [
    "en": ["who", "what", "why", "how", "when", "where", "which", "whose", "whom"],
    "it": [
      "chi", "cosa", "che", "perché", "perche", "come", "quando", "dove", "quale", "quali", "quanto", "quanta",
      "quanti", "quante",
    ],
    "fr": [
      "qui", "que", "quoi", "pourquoi", "comment", "quand", "où", "quel", "quelle", "quels", "quelles", "combien",
      "est-ce que", "qu'est-ce que",
    ],
    "de": [
      "wer", "was", "warum", "wieso", "weshalb", "wie", "wann", "wo", "welche", "welcher", "welches", "woher", "wohin",
      "wieviel",
    ],
    "es": [
      "qué", "quién", "quiénes", "cómo", "cuándo", "dónde", "cuál", "cuáles", "cuánto", "cuánta", "cuántos", "cuántas",
      "por qué",
    ],
    "pt": [
      "quem", "como", "quando", "onde", "qual", "quais", "quanto", "quanta", "quantos", "quantas", "o que", "por que",
      "por quê",
    ],
    "nl": ["wie", "wat", "waarom", "hoe", "wanneer", "waar", "welke", "hoeveel"],
  ]

  /// Whether `text` reads as a question, in English or in `locale`'s
  /// language: two words or more, not a URL, and ending with a question
  /// mark (also "？" or "؟"), starting with "¿", or starting with a question
  /// word ("what's" counts as "what"). The desktop's LooksLikeQuestion().
  public nonisolated static func isQuestion(_ text: String, locale: String) -> Bool {
    let lower = text.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    let words = lower.split(separator: " ", omittingEmptySubsequences: true)
    guard words.count >= 2, !lower.contains("://") else { return false }
    if let last = lower.unicodeScalars.last, ["?", "\u{FF1F}", "\u{061F}"].contains(last) { return true }
    if lower.unicodeScalars.first == "\u{00BF}" { return true }
    // The first word's letters ("what's" is "what").
    var first = ""
    for scalar in words[0].unicodeScalars {
      guard scalar.properties.isAlphabetic else { break }
      first.unicodeScalars.append(scalar)
    }
    let twoWords = words[0] + " " + words[1]
    let language = String(locale.prefix { $0 != "-" && $0 != "_" }).lowercased()
    let candidates = (questionWords["en"] ?? []) + (language == "en" ? [] : questionWords[language] ?? [])
    return candidates.contains { $0 == first || $0 == twoWords }
  }

  private nonisolated static func oneLine(_ text: String) -> String {
    text.replacingOccurrences(of: "\r", with: " ").replacingOccurrences(of: "\n", with: " ")
      .replacingOccurrences(of: "\t", with: " ").trimmingCharacters(in: .whitespaces)
  }

  private nonisolated static func cut(_ text: String, _ bytes: Int) -> String {
    guard text.utf8.count > bytes else { return text }
    var cut = ""
    for character in text {
      if cut.utf8.count + String(character).utf8.count > bytes { break }
      cut.append(character)
    }
    return cut + "\u{2026}"
  }
}
