// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassClaude

/// A Claude endpoint that answers from a script and counts what it got, as
/// the desktop's ClaudeQuickTest's fake server.
actor FakeClaude {
  var requests: [URLRequest] = []
  var status = 200
  var reply = #"{"name": "Rust Async"}"#
  var stopReason = "end_turn"
  var delay: Duration = .zero
  var unreachable = false

  func set(reply: String? = nil, status: Int? = nil, stopReason: String? = nil, delay: Duration? = nil, unreachable: Bool? = nil) {
    if let reply { self.reply = reply }
    if let status { self.status = status }
    if let stopReason { self.stopReason = stopReason }
    if let delay { self.delay = delay }
    if let unreachable { self.unreachable = unreachable }
  }

  func handle(_ request: URLRequest) async throws -> (Data, Int) {
    requests.append(request)
    if delay > .zero { try await Task.sleep(for: delay) }
    if unreachable { throw URLError(.cannotConnectToHost) }
    let message: JSONValue = [
      "type": "message", "role": "assistant", "stop_reason": .string(stopReason),
      "content": [["type": "text", "text": .string(reply)]],
    ]
    return (message.encoded(), status)
  }

  var count: Int { requests.count }
  var lastBody: JSONValue? { requests.last?.httpBody.flatMap(JSONValue.parse) }
}

@MainActor
final class QuickHarness {
  let fake: FakeClaude
  var key: String? = "sk-ant-api03-test-key-1234"
  var on: Set<String> = [ClaudeQuick.nameLinkFoldersKey]
  var quick: ClaudeQuick!

  init() {
    let fake = FakeClaude()
    self.fake = fake
    quick = ClaudeQuick(
      endpoint: { [unowned self] in self.key.map { ClaudeEndpoint(apiKey: $0) } },
      isOn: { [unowned self] in self.on.contains($0) },
      transport: { request in try await fake.handle(request) })
  }

  func request(background: Bool = true) -> ClaudeQuick.Request {
    ClaudeQuick.Request(
      feature: ClaudeQuick.nameLinkFoldersKey, model: "claude-haiku-5-5", system: "Name it.", user: "- A (a.example)\n",
      maxTokens: 60, background: background)
  }
}

@MainActor
struct ClaudeQuickTests {
  @Test func requestShape() {
    let request = ClaudeQuick.Request(feature: "f", system: "Prompt", user: "Tabs", maxTokens: 5000)
    let body = ClaudeQuick.body(request, model: "claude-haiku-5-5", cacheLevel: .normal, oauthToken: false)
    #expect(body["model"] == "claude-haiku-5-5")
    // Capped, no tools, no thinking, not streamed.
    #expect(body["max_tokens"]?.number == 400)
    #expect(body["tools"] == nil && body["thinking"] == nil && body["stream"] == nil)
    #expect(body["system"]?.array?.count == 1)
    #expect(body["system"]?.array?.first?["text"] == "Prompt")
    #expect(body["system"]?.array?.first?["cache_control"] != nil && body["cache_control"] != nil)
    #expect(body["messages"]?.array?.count == 1)
    #expect(body["messages"]?.array?.first?["role"] == "user")
    #expect(body["messages"]?.array?.first?["content"]?.array?.first?["text"] == "Tabs")
    #expect(ChatRequest.cacheBreakpoints(in: body) == 2)

    // A subscription token: Claude Code's line first. Fewer breakpoints
    // for a gateway that wants them.
    let oauth = ClaudeQuick.body(request, model: "m", cacheLevel: .topLevelOnly, oauthToken: true)
    #expect(oauth["system"]?.array?.first?["text"]?.string == ChatRequest.oauthSystemLine)
    #expect(ChatRequest.cacheBreakpoints(in: oauth) == 1)
    #expect(ChatRequest.cacheBreakpoints(in: ClaudeQuick.body(request, model: "m", cacheLevel: .none, oauthToken: false)) == 0)
  }

  @Test func parsesOnlyOneJSONObject() {
    #expect(ClaudeQuick.parseReply(#"  {"name": "A"} "#)?["name"] == "A")
    #expect(ClaudeQuick.parseReply(#"Sure! {"name": "A"}"#) == nil)
    #expect(ClaudeQuick.parseReply(#"{"name": "A"} and more"#) == nil)
    #expect(ClaudeQuick.parseReply(#"["A"]"#) == nil)
    #expect(ClaudeQuick.parseReply("") == nil)
  }

  @Test func sanitizesNames() {
    #expect(sanitizeAiName("Rust Async Runtimes") == "Rust Async Runtimes")
    #expect(sanitizeAiName("  “Flights to Lisbon.” ") == "Flights to Lisbon")
    #expect(sanitizeAiName("\"React   Hooks\"") == "React Hooks")
    #expect(sanitizeAiName("Voli per Lisbona") == "Voli per Lisbona")
    #expect(sanitizeAiName("東京 旅行") == "東京 旅行")
    #expect(sanitizeAiName("Top 10") == "Top 10")
    // Too long, too many words, nothing at all.
    #expect(sanitizeAiName("One Two Three Four") == nil)
    #expect(sanitizeAiName(String(repeating: "a", count: 33)) == nil)
    #expect(sanitizeAiName(String(repeating: "a", count: 32)) != nil)
    #expect(sanitizeAiName("  ") == nil)
    #expect(sanitizeAiName("...") == nil)
    // Emoji, line breaks, markup, format characters.
    #expect(sanitizeAiName("Trips ✈️") == nil)
    #expect(sanitizeAiName("Trips 🏖") == nil)
    #expect(sanitizeAiName("🇮🇹 Italy") == nil)
    #expect(sanitizeAiName("Line\nbreak") == nil)
    #expect(sanitizeAiName("<b>Bold</b>") == nil)
    #expect(sanitizeAiName("{name}") == nil)
    #expect(sanitizeAiName("Left\u{202E}Right") == nil)
  }

  @Test func readsPromptSections() throws {
    let text = """
      # Title

      Intro, not sent.

      ## folder_name

      Name the folder in {locale}.
        Keep indents.

      ## tidy_today

      Other.
      """
    #expect(SidebarPrompts.section("folder_name", in: text, locale: "it-IT") == "Name the folder in it-IT.\n  Keep indents.")
    #expect(SidebarPrompts.section("tidy_today", in: text, locale: "en") == "Other.")
    #expect(SidebarPrompts.section("missing", in: text, locale: "en") == "")

    // The shared file (bundled with the app) has the section, asking for JSON.
    let file = URL(fileURLWithPath: #filePath)
      .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
      .deletingLastPathComponent().deletingLastPathComponent()
      .appendingPathComponent("common/claude/sidebar_prompts.md")
    let prompt = SidebarPrompts.section(
      LinkFolderNamer.promptID, in: try String(contentsOf: file, encoding: .utf8), locale: "it-IT")
    #expect(prompt.contains(#"{"name":"#) && prompt.contains("it-IT") && !prompt.contains("{locale}"))
  }

  @Test func asksAndReturnsTheJSON() async {
    let h = QuickHarness()
    let result = await h.quick.ask(h.request())
    #expect(result == .success(["name": "Rust Async"]))
    #expect(await h.fake.count == 1)
    let request = await h.fake.requests.first!
    #expect(request.url?.absoluteString == "https://api.anthropic.com/v1/messages")
    #expect(request.value(forHTTPHeaderField: "x-api-key") == "sk-ant-api03-test-key-1234")
    let body = await h.fake.lastBody
    #expect(body?["model"] == "claude-haiku-5-5" && body?["max_tokens"]?.number == 60 && body?["tools"] == nil)
    // An empty model is the default one.
    var request2 = h.request()
    request2.model = ""
    _ = await h.quick.ask(request2)
    #expect(await h.fake.lastBody?["model"]?.string == ClaudeQuick.defaultModel)
  }

  @Test func nothingIsSentWithTheSwitchOff() async {
    let h = QuickHarness()
    h.on = []
    #expect(await h.quick.ask(h.request()) == .failure(.off))
    #expect(await h.quick.ask(h.request(background: false)) == .failure(.off))
    h.on = [ClaudeQuick.nameLinkFoldersKey]
    h.key = nil
    #expect(await h.quick.ask(h.request()) == .failure(.notSetUp))
    // A subscription token without its risks accepted isn't set up.
    h.key = "sk-ant-oat01-abcdefghijkl"
    #expect(!h.quick.isSetUp)
    #expect(await h.quick.ask(h.request()) == .failure(.notSetUp))
    #expect(await h.fake.count == 0)
  }

  @Test func refusalsAndBadRepliesDontBackOff() async {
    let h = QuickHarness()
    await h.fake.set(stopReason: "refusal")
    #expect(await h.quick.ask(h.request()) == .failure(.refused))
    await h.fake.set(reply: #"Here: {"name": "A"}"#, stopReason: "end_turn")
    #expect(await h.quick.ask(h.request()) == .failure(.badReply))
    await h.fake.set(reply: #"{"name": "B"}"#)
    #expect(await h.quick.ask(h.request()) == .success(["name": "B"]))
  }

  @Test func errorsBackOff() async {
    let h = QuickHarness()
    var clock = Date(timeIntervalSince1970: 1_000_000)
    h.quick.now = { clock }
    await h.fake.set(status: 529)
    #expect(await h.quick.ask(h.request()) == .failure(.failed))
    #expect(await h.quick.ask(h.request()) == .failure(.backingOff))
    // One the user asked for goes anyway (and fails again: 10 min now).
    #expect(await h.quick.ask(h.request(background: false)) == .failure(.failed))
    clock.addTimeInterval(61)
    #expect(await h.quick.ask(h.request()) == .failure(.backingOff))
    clock.addTimeInterval(10 * 60)
    await h.fake.set(status: 200, unreachable: true)
    #expect(await h.quick.ask(h.request()) == .failure(.failed))
    clock.addTimeInterval(10 * 60 + 1)
    await h.fake.set(unreachable: false)
    #expect(await h.quick.ask(h.request()) == .success(["name": "Rust Async"]))
    #expect(await h.fake.count == 4)
  }

  @Test func timesOut() async {
    let h = QuickHarness()
    h.quick.timeout = 0.2
    await h.fake.set(delay: .seconds(5))
    #expect(await h.quick.ask(h.request()) == .failure(.timeout))
  }

  @Test func limitsBackgroundRequestsPerHour() async {
    let h = QuickHarness()
    var clock = Date(timeIntervalSince1970: 1_000_000)
    h.quick.now = { clock }
    for _ in 0..<ClaudeQuick.maxBackgroundPerHour {
      #expect(await h.quick.ask(h.request()) == .success(["name": "Rust Async"]))
      clock.addTimeInterval(10)
    }
    #expect(h.quick.recentBackgroundRequests == 30)
    #expect(await h.quick.ask(h.request()) == .failure(.rateLimited))
    // Suggest a Name isn't limited.
    #expect(await h.quick.ask(h.request(background: false)) == .success(["name": "Rust Async"]))
    clock.addTimeInterval(60 * 60)
    #expect(await h.quick.ask(h.request()) == .success(["name": "Rust Async"]))
    #expect(await h.fake.count == 32)
  }
}
