// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassClaude

/// Ask in the command bar (docs/design/ai-sidebar.md, A5 on iOS), with a
/// fake streaming client.
@MainActor
struct CommandBarAskerTests {
  static let prompts = """
    ## tidy_today

    Sort.

    ## command_bar_ask

    Answer briefly ({locale}).
    """

  /// A streaming client that records each request and plays `events`.
  @MainActor final class FakeStream {
    var bodies: [JSONValue] = []
    var events: [(String, String)] = []
    var error: String?
    var delay: Duration = .zero

    static func delta(_ text: String) -> (String, String) {
      let data: JSONValue = ["type": "content_block_delta", "index": 0, "delta": ["type": "text_delta", "text": .string(text)]]
      return ("content_block_delta", String(decoding: data.encoded(), as: UTF8.self))
    }

    var stream: CommandBarAsker.Stream {
      { [unowned self] body, _, onEvent in
        self.bodies.append(body)
        for event in self.events {
          if self.delay > .zero { try? await Task.sleep(for: self.delay) }
          if Task.isCancelled { return nil }
          onEvent(event.0, event.1)
        }
        return self.error
      }
    }
  }

  @MainActor final class Setup {
    let harness = QuickHarness()
    let fake = FakeStream()
    let asker: CommandBarAsker

    init(on: Bool = true) {
      harness.on = on ? [ClaudeQuick.commandBarAskKey] : []
      asker = CommandBarAsker(
        quick: harness.quick, prompts: CommandBarAskerTests.prompts, model: { "claude-sonnet-5-5" },
        locale: { "it-IT" }, stream: fake.stream)
      fake.events = [
        ("message_start", "{}"), FakeStream.delta("Lisbon is "), FakeStream.delta("Portugal's capital."),
        ("message_stop", "{}"),
      ]
    }

    func settle() async { try? await Task.sleep(for: .milliseconds(150)) }
  }

  // MARK: Questions

  /// The desktop's ClaudeQuickTest.LooksLikeQuestion, and more.
  @Test func detectsQuestions() {
    #expect(CommandBarAsker.isQuestion("is it raining in Lisbon?", locale: "en-US"))
    #expect(CommandBarAsker.isQuestion("  what is a fox  ", locale: "en-US"))
    #expect(CommandBarAsker.isQuestion("What's the capital of Peru", locale: "en-US"))
    #expect(CommandBarAsker.isQuestion("How do foxes hunt", locale: "it-IT"))
    #expect(CommandBarAsker.isQuestion("cosa mangiano le volpi", locale: "it-IT"))
    #expect(CommandBarAsker.isQuestion("Perch\u{E9} il cielo \u{E8} blu", locale: "it"))
    #expect(CommandBarAsker.isQuestion("por qu\u{E9} llueve", locale: "es-419"))
    #expect(CommandBarAsker.isQuestion("\u{BF}llueve hoy en Madrid", locale: "en-US"))
    #expect(CommandBarAsker.isQuestion("wie sp\u{E4}t ist es", locale: "de-DE"))
    #expect(CommandBarAsker.isQuestion("waarom is de lucht blauw", locale: "nl-NL"))
    #expect(CommandBarAsker.isQuestion("东京 在哪里？", locale: "zh-CN"))
    // Another language's words count only in that language.
    #expect(!CommandBarAsker.isQuestion("cosa mangiano le volpi", locale: "en-US"))
    #expect(!CommandBarAsker.isQuestion("come together lyrics", locale: "en-US"))
    // Searches, URLs and single words aren't questions; nor verbs without "?".
    #expect(!CommandBarAsker.isQuestion("fox facts", locale: "en-US"))
    #expect(!CommandBarAsker.isQuestion("what?", locale: "en-US"))
    #expect(!CommandBarAsker.isQuestion("https://example.com/?q=what is", locale: "en-US"))
    #expect(!CommandBarAsker.isQuestion("", locale: "en-US"))
    #expect(!CommandBarAsker.isQuestion("whatever works", locale: "en-US"))
    #expect(!CommandBarAsker.isQuestion("can opener", locale: "en-US"))
  }

  /// The desktop's ClaudeQuickTest.DescribeAskPage.
  @Test func describesThePage() {
    #expect(CommandBarAsker.describePage(title: "", host: "") == "")
    #expect(CommandBarAsker.describePage(title: "Fox\npage", host: "fox.test")
      == "I'm looking at a page titled \"Fox page\" on fox.test.")
    #expect(CommandBarAsker.describePage(title: "", host: "fox.test") == "I'm looking at a page on fox.test.")
  }

  @Test func offersOnlyWithTheSwitchOnAndNotInIncognito() {
    let s = Setup()
    #expect(s.asker.offers("what is a monad", incognito: false))
    #expect(!s.asker.offers("monads", incognito: false))
    #expect(!s.asker.offers("what is a monad", incognito: true))
    s.harness.on = []
    #expect(!s.asker.offers("what is a monad", incognito: false))
    s.harness.on = [ClaudeQuick.commandBarAskKey]
    s.harness.key = nil
    #expect(!s.asker.offers("what is a monad", incognito: false))
  }

  // MARK: Asking

  @Test func streamsTheAnswer() async {
    let s = Setup()
    s.asker.ask("  what is the capital of Portugal? ", page: nil, incognito: false)
    #expect(s.asker.state == .asking && s.asker.question == "what is the capital of Portugal?")
    await s.settle()
    #expect(s.asker.answer == "Lisbon is Portugal's capital.")
    #expect(s.asker.state == .answered)
    #expect(s.fake.bodies.count == 1)
    // The user's request: no hourly limit counted.
    #expect(s.harness.quick.recentBackgroundRequests == 0)
  }

  @Test func requestShape() async {
    let s = Setup()
    s.asker.ask("who wrote this?", page: (title: "The Name of the Rose\nreview", host: "example.org"), incognito: false)
    await s.settle()
    let body = s.fake.bodies.first
    #expect(body?["model"] == "claude-sonnet-5-5")
    #expect(body?["stream"] == true)
    #expect(body?["tools"] == nil && body?["thinking"] == nil)
    #expect(body?["max_tokens"]?.number == Double(CommandBarAsker.maxTokens))
    #expect(body?["system"]?.array?.last?["text"] == "Answer briefly (it-IT).")
    #expect(body.map(ChatRequest.cacheBreakpoints(in:)) == 2)
    let messages = body?["messages"]?.array ?? []
    #expect(messages.count == 1 && messages.first?["role"] == "user")
    let user = messages.first?["content"]?.array?.first?["text"]?.string ?? ""
    // The page's title and host only, on one line, then the question.
    #expect(user == "I'm looking at a page titled \"The Name of the Rose review\" on example.org.\n\nwho wrote this?")
    #expect(s.asker.message == user)

    // Without "About this page": the question alone.
    #expect(CommandBarAsker.userMessage(question: "why?", page: nil) == "why?")
    // A subscription token: Claude Code's line first; a gateway's level.
    let oauth = CommandBarAsker.body(
      question: "q?", page: nil, system: "P", model: "m", cacheLevel: .topLevelOnly, oauthToken: true)
    #expect(oauth["system"]?.array?.first?["text"]?.string == ChatRequest.oauthSystemLine)
    #expect(ChatRequest.cacheBreakpoints(in: oauth) == 1)
  }

  @Test func nothingSentWithTheSwitchOffOrInIncognito() async {
    let s = Setup(on: false)
    s.asker.ask("what is a monad?", page: nil, incognito: false)
    await s.settle()
    #expect(s.fake.bodies.isEmpty && s.asker.state == .idle)

    s.harness.on = [ClaudeQuick.commandBarAskKey]
    s.asker.ask("what is a monad?", page: (title: "Private", host: "secret.example"), incognito: true)
    await s.settle()
    #expect(s.fake.bodies.isEmpty && s.asker.state == .idle)

    s.harness.key = nil
    s.asker.ask("what is a monad?", page: nil, incognito: false)
    await s.settle()
    #expect(s.fake.bodies.isEmpty)
  }

  @Test func errorsAndCancel() async {
    let s = Setup()
    s.fake.events = [FakeStream.delta("Partial")]
    s.fake.error = "Claude couldn't be reached."
    s.asker.ask("why is the sky blue", page: nil, incognito: false)
    await s.settle()
    #expect(s.asker.state == .failed("Claude couldn't be reached.") && s.asker.answer == "Partial")

    // Nothing came back.
    s.fake.events = []
    s.fake.error = nil
    s.asker.ask("why is the sky blue", page: nil, incognito: false)
    await s.settle()
    #expect(s.asker.state == .failed("Claude didn't answer."))

    // Cancelled while streaming: late events drop.
    s.fake.events = [FakeStream.delta("A"), FakeStream.delta("B"), FakeStream.delta("C")]
    s.fake.delay = .milliseconds(60)
    s.asker.ask("why is the sky blue", page: nil, incognito: false)
    try? await Task.sleep(for: .milliseconds(90))
    s.asker.cancel()
    await s.settle()
    #expect(s.asker.state == .idle && s.asker.answer.isEmpty)

    // No answer in time: what came stays, with the error.
    s.asker.timeout = .milliseconds(100)
    s.fake.delay = .milliseconds(80)
    s.asker.ask("why is the sky blue", page: nil, incognito: false)
    try? await Task.sleep(for: .milliseconds(200))
    #expect(s.asker.state == .failed("Claude took too long.") && s.asker.answer == "A")
    await s.settle()
    #expect(s.asker.answer == "A")
    s.asker.timeout = .seconds(60)
    s.fake.delay = .milliseconds(60)

    // Turned off while it streams: the answer stops.
    s.asker.ask("why is the sky blue", page: nil, incognito: false)
    try? await Task.sleep(for: .milliseconds(90))
    s.harness.on = []
    await s.settle()
    #expect(s.asker.state == .idle)
  }

  @Test func textDeltas() {
    let delta = FakeStream.delta("Hi")
    #expect(CommandBarAsker.textDelta(delta.0, delta.1) == "Hi")
    #expect(CommandBarAsker.textDelta("content_block_delta", #"{"delta": {"type": "thinking_delta", "thinking": "x"}}"#) == nil)
    #expect(CommandBarAsker.textDelta("message_stop", "{}") == nil)
  }

  @Test func continueInChatSeedsTheConversation() {
    var conversation = Conversation()
    conversation.addUser(text: "what is a monad?")
    conversation.addAssistant(text: "A monoid in the category of endofunctors.")
    let messages = conversation.messages()
    #expect(messages.count == 2)
    #expect(messages.last?["role"] == "assistant")
    #expect(messages.last?["content"]?.array?.first?["text"] == "A monoid in the category of endofunctors.")
    // The next message follows them.
    conversation.addUser(text: "an example?")
    #expect(conversation.messages().count == 3)
  }

  @Test func theSharedPromptHasItsSection() throws {
    let url = URL(fileURLWithPath: #filePath)
      .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
      .deletingLastPathComponent().deletingLastPathComponent()
      .appendingPathComponent("common/claude/sidebar_prompts.md")
    let prompts = try String(contentsOf: url, encoding: .utf8)
    let section = SidebarPrompts.section(CommandBarAsker.promptID, in: prompts, locale: "it-IT")
    #expect(!section.isEmpty && section.contains("it-IT") && !section.contains("## "))
  }
}
