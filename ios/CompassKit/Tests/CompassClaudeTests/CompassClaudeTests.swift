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
