// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import Foundation
import Testing

@testable import CompassClaude

struct ClaudeEndpointTests {
  @Test func baseURLRules() {
    #expect(ClaudeEndpoint.parseBaseURL("") == ClaudeEndpoint.defaultBaseURL)
    #expect(ClaudeEndpoint.parseBaseURL(" https://gw.example.com/anthropic/ ")?.absoluteString == "https://gw.example.com/anthropic")
    #expect(ClaudeEndpoint.parseBaseURL("https://gw.example.com/")?.absoluteString == "https://gw.example.com")
    #expect(ClaudeEndpoint.parseBaseURL("http://localhost:8080")?.absoluteString == "http://localhost:8080")
    // The key goes with every request: never in the clear, never with
    // credentials, a query or a fragment.
    #expect(ClaudeEndpoint.parseBaseURL("http://gw.example.com") == nil)
    #expect(ClaudeEndpoint.parseBaseURL("https://u:p@gw.example.com") == nil)
    #expect(ClaudeEndpoint.parseBaseURL("https://gw.example.com/?a=1") == nil)
    #expect(ClaudeEndpoint.parseBaseURL("https://gw.example.com/#x") == nil)
    #expect(ClaudeEndpoint.parseBaseURL("not a url") == nil)
  }

  @Test func oauthTokenGetsClaudeCodeLineFirst() {
    func system(_ oauth: Bool) -> [JSONValue] {
      let options = ChatRequest.Options(model: "m", systemPrompt: "p", claudeAPI: true, oauthToken: oauth)
      return ChatRequest(messages: [["role": "user", "content": "x"]], options: options).body["system"]?.array ?? []
    }
    #expect(system(true).count == system(false).count + 1)
    #expect(system(true).first?["text"]?.string == "You are Claude Code, Anthropic's official CLI for Claude.")
    #expect(system(false).first?["text"]?.string == "p")
  }

  @Test func oauthTokenFromSetupToken() {
    // Refused until the risks are accepted.
    let refused = ClaudeEndpoint(apiKey: "sk-ant-oat01-abc")
    #expect(refused.isOAuthToken)
    #expect(refused.needsTokenRisksAccepted)
    #expect(!refused.usesOAuthToken)
    #expect(refused.request("v1/models").value(forHTTPHeaderField: "anthropic-beta") == nil)

    let token = ClaudeEndpoint(apiKey: "sk-ant-oat01-abc", allowOAuthToken: true)
    #expect(token.usesOAuthToken)
    #expect(!token.needsTokenRisksAccepted)
    let request = token.request("v1/messages", body: Data("{}".utf8), betas: ["a"])
    #expect(request.value(forHTTPHeaderField: "Authorization") == "Bearer sk-ant-oat01-abc")
    #expect(request.value(forHTTPHeaderField: "x-api-key") == nil)
    #expect(request.value(forHTTPHeaderField: "anthropic-beta") == "a,oauth-2025-04-20")
    #expect(token.request("v1/models").value(forHTTPHeaderField: "anthropic-beta") == "oauth-2025-04-20")
    let key = ClaudeEndpoint(apiKey: "sk-ant-api03-abc", allowOAuthToken: true)
    #expect(!key.isOAuthToken)
    #expect(!key.needsTokenRisksAccepted)
    #expect(key.request("v1/models").value(forHTTPHeaderField: "anthropic-beta") == nil)
  }

  @Test func requestHeaders() {
    let key = ClaudeEndpoint(apiKey: "sk-ant-test")
    let request = key.request("v1/messages", body: Data("{}".utf8), betas: ["a", "b"])
    #expect(request.url?.absoluteString == "https://api.anthropic.com/v1/messages")
    #expect(request.httpMethod == "POST")
    #expect(request.value(forHTTPHeaderField: "x-api-key") == "sk-ant-test")
    #expect(request.value(forHTTPHeaderField: "anthropic-version") == "2026-10-21")
    #expect(request.value(forHTTPHeaderField: "anthropic-beta") == "a,b")
    let bearer = ClaudeEndpoint(baseURL: URL(string: "https://gw.example.com/api")!, authStyle: .bearer, apiKey: "t")
    let get = bearer.request("v1/models?limit=100")
    #expect(get.url?.absoluteString == "https://gw.example.com/api/v1/models?limit=100")
    #expect(get.httpMethod == "GET")
    #expect(get.value(forHTTPHeaderField: "Authorization") == "Bearer t")
    #expect(get.value(forHTTPHeaderField: "x-api-key") == nil)
  }

  @Test func errorsAndHints() {
    let body = Data(#"{"type":"error","error":{"type":"invalid_request_error","message":"Bad thing."}}"#.utf8)
    #expect(ClaudeEndpoint.describeError(status: 400, body: body) == "Bad thing.")
    #expect(ClaudeEndpoint.describeError(status: 401, body: Data()).contains("401"))
    #expect(ClaudeEndpoint.describeError(status: 0, body: Data()).hasPrefix("Couldn't reach"))
    #expect(ClaudeEndpoint.keyHint("sk-ant-api03-abcdefghijkl1234") == "sk-ant-…1234")
    #expect(ClaudeEndpoint.keyHint("short") == "…")
  }

  @Test func modelList() {
    let data = Data(#"{"data":[{"id":"claude-opus-5-5","display_name":"Claude Opus 5.5"},{"id":"x"},{"id":""}]}"#.utf8)
    #expect(ClaudeEndpoint.parseModels(data) == [
      ClaudeModel(id: "claude-opus-5-5", displayName: "Claude Opus 5.5"), ClaudeModel(id: "x", displayName: "x"),
    ])
    #expect(ClaudeEndpoint.parseModels(Data("[]".utf8)) == nil)
  }
}

struct ChatRequestTests {
  /// Everything that can carry a breakpoint at once (a token's line,
  /// memory, files, developer tools, tools, a page, earlier tool results)
  /// stays within the API's 4; minimal caching keeps only the top-level one.
  @Test func cacheBreakpointsStayWithinTheLimit() {
    let tools: [JSONValue] = [
      ["name": "read_page", "description": "d", "input_schema": ["type": "object"]],
      ["name": "remember", "description": "d", "input_schema": ["type": "object"]],
    ]
    let messages: [JSONValue] = [
      ["role": "user", "content": [["type": "text", "text": "<page>…</page>"], ["type": "text", "text": "hi"]]],
      ["role": "assistant", "content": [["type": "tool_use", "id": "t1", "name": "read_page", "input": [:]]]],
      ["role": "user", "content": [["type": "tool_result", "tool_use_id": "t1", "content": [["type": "text", "text": "ok"]]]]],
    ]
    var options = ChatRequest.Options(
      model: "claude-opus-5-5", systemPrompt: "PROMPT", tools: tools, browserScope: true, windowTabs: "- a — https://a.test/",
      claudeAPI: true, oauthToken: true, memory: "<memory>\n- x\n</memory>", files: "Files: on.", devtools: true)
    let full = ChatRequest(messages: messages, options: options).body
    #expect(ChatRequest.cacheBreakpoints(in: full) == 2)
    #expect(ChatRequest.cacheBreakpoints(in: full) <= ChatRequest.maxCacheBreakpoints)
    options.cacheLevel = .topLevelOnly
    let minimal = ChatRequest(messages: messages, options: options).body
    #expect(ChatRequest.cacheBreakpoints(in: minimal) == 1)
    #expect(minimal["cache_control"] == ["type": "ephemeral"])
    #expect(minimal["system"]?.array?.allSatisfy { $0["cache_control"] == nil } == true)
    options.cacheLevel = .none
    let none = ChatRequest(messages: messages, options: options).body
    #expect(ChatRequest.cacheBreakpoints(in: none) == 0)
    // The same request otherwise.
    #expect(none["system"]?.array?.count == full["system"]?.array?.count)
    #expect(none["messages"] == full["messages"])
  }

  @Test func cacheLevelsStepDown() {
    #expect(ChatRequest.CacheLevel.normal.lower == .topLevelOnly)
    #expect(ChatRequest.CacheLevel.topLevelOnly.lower == ChatRequest.CacheLevel.none)
    #expect(ChatRequest.CacheLevel.none.lower == nil)
  }

  @Test func recognizesTheTooManyBreakpointsError() {
    #expect(ChatRequest.isTooManyCacheBreakpoints("A maximum of 4 blocks with cache_control may be provided. Found 5."))
    #expect(ChatRequest.isTooManyCacheBreakpoints("A MAXIMUM OF 4 BLOCKS WITH CACHE_CONTROL MAY BE PROVIDED"))
    // Wrapped by a gateway.
    #expect(ChatRequest.isTooManyCacheBreakpoints(
      #"upstream error 400: {"type":"error","error":{"type":"invalid_request_error","message":"A maximum of 4 blocks with cache_control may be provided. Found 6."}}"#))
    #expect(ChatRequest.isTooManyCacheBreakpoints("Too many cache_control breakpoints"))
    #expect(!ChatRequest.isTooManyCacheBreakpoints("Overloaded"))
    #expect(!ChatRequest.isTooManyCacheBreakpoints("cache_control: unknown field"))
    #expect(!ChatRequest.isTooManyCacheBreakpoints(nil))
    // A 400 body in a gateway's own shape, or plain text, keeps the words.
    let wrapped = Data(#"{"detail":"Anthropic: A maximum of 4 blocks with cache_control may be provided. Found 7."}"#.utf8)
    #expect(ChatRequest.isTooManyCacheBreakpoints(ClaudeEndpoint.describeError(status: 400, body: wrapped)))
    let plain = Data("Bad request: maximum of 4 blocks with cache_control exceeded".utf8)
    #expect(ChatRequest.isTooManyCacheBreakpoints(ClaudeEndpoint.describeError(status: 400, body: plain)))
    #expect(ClaudeEndpoint.describeError(status: 400, body: Data("<html>nope</html>".utf8)) == "The server answered with HTTP 400.")
  }

  @Test func cacheLevelRememberedPerEndpoint() throws {
    let defaults = try #require(UserDefaults(suiteName: "CacheLevelMemoryTests"))
    defaults.removePersistentDomain(forName: "CacheLevelMemoryTests")
    let memory = CacheLevelMemory(defaults: defaults)
    let gateway = URL(string: "https://gateway.test/anthropic")!
    let api = URL(string: "https://api.anthropic.com")!
    #expect(memory.level(for: gateway, isClaudeAPI: false) == .normal)
    memory.remember(.none, for: gateway, isClaudeAPI: false)
    #expect(memory.level(for: gateway, isClaudeAPI: false) == ChatRequest.CacheLevel.none)
    // Another URL forgets it.
    #expect(memory.level(for: URL(string: "https://other.test")!, isClaudeAPI: false) == .normal)
    #expect(memory.level(for: gateway, isClaudeAPI: false) == .normal)
    // Never for the Claude API.
    memory.remember(.topLevelOnly, for: api, isClaudeAPI: true)
    #expect(memory.level(for: api, isClaudeAPI: true) == .normal)
    // The older key: minimal caching for its URL.
    defaults.set(gateway.absoluteString, forKey: "ClaudeMinimalCachingURL")
    #expect(memory.level(for: gateway, isClaudeAPI: false) == .topLevelOnly)
    #expect(defaults.string(forKey: "ClaudeMinimalCachingURL") == nil)
    defaults.removePersistentDomain(forName: "CacheLevelMemoryTests")
  }

  /// A fake gateway: refuses with the API's words while the request has more
  /// than `allowed` breakpoints of ours.
  @MainActor
  final class FakeGateway {
    let allowed: Int
    var requests: [JSONValue] = []
    init(allowed: Int) { self.allowed = allowed }
    func send(_ request: ChatRequest) async -> String? {
      requests.append(request.body)
      if (request.body["messages"]?.array ?? []).isEmpty { return "messages: at least one message is required" }
      let ours = ChatRequest.cacheBreakpoints(in: request.body)
      return ours > allowed ? "A maximum of 4 blocks with cache_control may be provided. Found \(ours + 4)." : nil
    }
  }

  @MainActor @Test func stepsDownWithTheSameMessages() async {
    var conversation = Conversation()
    conversation.addUser(text: "Check why auto pip is failing")
    let messages = conversation.messages()
    conversation.startReply()
    let options = ChatRequest.Options(model: "claude-opus-5-5", systemPrompt: "PROMPT", claudeAPI: false)
    // A gateway that adds 4: only "none" gets through.
    let gateway = FakeGateway(allowed: 0)
    let outcome = await ChatRequest.send(
      messages: messages, options: options, restart: { conversation.restartReply() }, send: gateway.send)
    #expect(outcome.error == nil)
    #expect(outcome.level == ChatRequest.CacheLevel.none)
    #expect(outcome.steppedDown)
    #expect(gateway.requests.count == 3)
    for body in gateway.requests {
      #expect(body["messages"]?.array?.isEmpty == false)
      #expect(body["messages"] == .array(messages))
    }
    conversation.finishReply(error: outcome.error)
    #expect(conversation.turns.allSatisfy { $0.error == nil })
    #expect(conversation.turns.count == 2)

    // One that adds 3: the top-level one gets through, in two requests.
    let three = FakeGateway(allowed: 1)
    let second = await ChatRequest.send(messages: messages, options: options, send: three.send)
    #expect(second.error == nil && second.level == .topLevelOnly && three.requests.count == 2)
    // Not a breakpoint refusal: no step down, its error.
    let refused = await ChatRequest.send(messages: messages, options: options) { _ in "Overloaded" }
    #expect(refused.error == "Overloaded" && !refused.steppedDown)
    // Something showed already: no step down.
    let shown = await ChatRequest.send(messages: messages, options: options, canStepDown: { false }, send: gateway.send)
    #expect(shown.error != nil && shown.level == .normal)
  }

  /// Retry after a failed reply sends the message again (it sent none).
  @Test func retryResendsTheMessage() {
    var conversation = Conversation()
    conversation.addUser(text: "hi")
    conversation.startReply()
    conversation.finishReply(error: "A maximum of 4 blocks with cache_control may be provided. Found 5.")
    #expect(conversation.messages().isEmpty)
    conversation.removeFailedReply()
    #expect(conversation.messages().count == 1)
    #expect(conversation.messages().first?["role"] == "user")
    #expect(conversation.turns.allSatisfy { $0.error == nil })
  }

  @Test func restartReplyKeepsNothingAndThinkingIsNoOutput() {
    var conversation = Conversation()
    conversation.addUser(text: "hi")
    conversation.startReply()
    conversation.apply(event: "content_block_start", data: #"{"index":0,"content_block":{"type":"thinking","thinking":""}}"#)
    conversation.apply(event: "content_block_delta", data: #"{"index":0,"delta":{"type":"thinking_delta","thinking":"hm"}}"#)
    #expect(!conversation.streamingTurnHasOutput)
    conversation.apply(event: "content_block_start", data: #"{"index":1,"content_block":{"type":"text","text":""}}"#)
    conversation.apply(event: "content_block_delta", data: #"{"index":1,"delta":{"type":"text_delta","text":"Hi"}}"#)
    #expect(conversation.streamingTurnHasOutput)
    conversation.restartReply()
    #expect(conversation.streamingTurn?.content.isEmpty == true)
    #expect(!conversation.streamingTurnHasOutput)
  }

  @Test func bodyAsTheDesktopBuildsIt() {
    let options = ChatRequest.Options(model: "claude-opus-5-5", systemPrompt: "PROMPT", claudeAPI: true)
    let request = ChatRequest(messages: [["role": "user", "content": "hi"]], options: options)
    let body = request.body
    #expect(body["model"] == "claude-opus-5-5")
    #expect(body["max_tokens"] == 64000)
    #expect(body["stream"] == true)
    #expect(body["system"]?.array?.first?["text"] == "PROMPT")
    #expect(body["system"]?.array?.first?["cache_control"] == ["type": "ephemeral"])
    #expect(body["system"]?.array?[1]["cache_control"] == nil)
    #expect(body["cache_control"] == ["type": "ephemeral"])
    #expect(body["thinking"] == ["type": "adaptive", "display": "summarized"])
    #expect(body["tools"] == nil)
    #expect(body["temperature"] == nil)
    #expect(body["fallbacks"] == "default")
    #expect(request.betas == ["server-side-fallback-2026-07-01"])
  }

  @Test func fallbacksOnlyOnTheClaudeAPIForTheirModels() {
    let gateway = ChatRequest(messages: [], options: .init(model: "claude-opus-5-5", systemPrompt: "", claudeAPI: false))
    #expect(gateway.body["fallbacks"] == nil)
    #expect(gateway.betas.isEmpty)
    let haiku = ChatRequest(messages: [], options: .init(model: "claude-haiku-5-5", systemPrompt: "", claudeAPI: true))
    #expect(haiku.body["fallbacks"] == nil)
  }

  @Test func sameRequestSameBytes() {
    let options = ChatRequest.Options(model: "m", systemPrompt: "p", claudeAPI: false, now: Date(timeIntervalSince1970: 0))
    let a = ChatRequest(messages: [["role": "user", "content": "x"]], options: options).body.encoded()
    let b = ChatRequest(messages: [["role": "user", "content": "x"]], options: options).body.encoded()
    #expect(a == b)
  }
}

struct ConversationTests {
  /// Feeds an SSE transcript through the event-stream parser.
  func stream(_ text: String, into conversation: inout Conversation) {
    var parser = EventStreamParser(maxLine: 1 << 20)
    for byte in Array(text.utf8) {
      for event in parser.feed(byte) { conversation.apply(event: event.name, data: event.data) }
    }
  }

  @Test func streamedReplyBecomesBlocks() {
    var c = Conversation()
    c.addUser(text: "Hello")
    c.startReply()
    stream(
      """
      event: message_start
      data: {"type":"message_start","message":{"id":"m"}}

      event: content_block_start
      data: {"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}}

      event: content_block_delta
      data: {"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Hmm"}}

      event: content_block_delta
      data: {"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"SIG"}}

      event: content_block_start
      data: {"type":"content_block_start","index":1,"content_block":{"type":"text","text":""}}

      event: content_block_delta
      data: {"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"Hi "}}

      event: content_block_delta
      data: {"type":"content_block_delta","index":1,"delta":{"type":"text_delta","text":"there"}}

      event: content_block_start
      data: {"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"t1","name":"read_page","input":{}}}

      event: content_block_delta
      data: {"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"{\\"filter\\":"}}

      event: content_block_delta
      data: {"type":"content_block_delta","index":2,"delta":{"type":"input_json_delta","partial_json":"\\"all\\"}"}}

      event: content_block_stop
      data: {"type":"content_block_stop","index":2}

      event: message_delta
      data: {"type":"message_delta","delta":{"stop_reason":"tool_use"}}


      """, into: &c)
    c.finishReply()
    let reply = c.turns.last!
    #expect(reply.stopReason == "tool_use")
    #expect(reply.content[0] == ["type": "thinking", "thinking": "Hmm", "signature": "SIG"])
    #expect(reply.content[1]["text"] == "Hi there")
    #expect(reply.content[2]["input"] == ["filter": "all"])
    // Tool calls left open get an error result before the next message.
    c.closeOpenTools("Not run.")
    let messages = c.messages()
    #expect(messages.count == 3)
    #expect(messages[2]["content"]?.array?.first?["tool_use_id"] == "t1")
    #expect(messages[2]["content"]?.array?.first?["is_error"] == true)
  }

  @Test func failedAndUnsignedAreLeftOut() {
    var c = Conversation()
    c.addUser(text: "first")
    c.startReply()
    c.finishReply(error: "boom")  // failed, nothing streamed: both go
    c.addUser(text: "second")
    c.startReply()
    c.apply(event: "content_block_start", data: #"{"index":0,"content_block":{"type":"thinking","thinking":"x"}}"#)
    c.apply(event: "content_block_start", data: #"{"index":1,"content_block":{"type":"text","text":"ok"}}"#)
    c.finishReply()  // stopped mid-thought: no signature
    let messages = c.messages()
    #expect(messages.count == 2)
    #expect(messages[0]["content"]?.array?.last?["text"] == "second")
    #expect(messages[1]["content"]?.array?.count == 1)
    #expect(messages[1]["content"]?.array?.first?["type"] == "text")
  }

  @Test func pageAttachedBeforeTheMessage() {
    var c = Conversation()
    c.addUser(text: "Summarize", page: (url: "https://a.example/?x=1&y=2", title: "A \"page\"", text: "Body", truncated: true))
    let content = c.messages()[0]["content"]!.array!
    #expect(content.count == 2)
    #expect(content[0]["text"] == "<page url=\"https://a.example/?x=1&amp;y=2\" title=\"A &quot;page&quot;\" truncated=\"true\">\nBody\n</page>")
    #expect(content[1]["text"] == "Summarize")
    #expect(c.turns[0].pageTitle == "A \"page\"")
  }

  @Test func onlyTheLatestScreenshotsStay() {
    var c = Conversation()
    c.addUser(text: "go")
    for i in 0..<3 {
      c.startReply()
      c.apply(event: "content_block_start", data: #"{"index":0,"content_block":{"type":"tool_use","id":"t\#(i)","name":"screenshot","input":{}}}"#)
      c.finishReply()
      c.addToolResults([["type": "tool_result", "tool_use_id": .string("t\(i)"), "content": [["type": "image", "source": [:]]]]])
    }
    let results = c.messages().filter { $0["role"] == "user" }.dropFirst().compactMap { $0["content"]?.array?.first?["content"]?.array?.first?["type"]?.string }
    #expect(results == ["text", "image", "image"])
  }
}
