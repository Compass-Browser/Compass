// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassClaude

/// docs/design/ai-providers.md: the Messages API as Chat Completions and
/// back.
struct OpenAIChatTests {
  /// A chat request as `ChatRequest` builds it, with a tool round and a
  /// screenshot.
  private func chatBody() -> JSONValue {
    var conversation = Conversation()
    conversation.addUser(text: "Click the button", page: (url: "https://a.example/", title: "A", text: "Hello", truncated: false))
    conversation.startReply()
    for (name, data) in [
      ("content_block_start", #"{"index":0,"content_block":{"type":"thinking","thinking":""}}"#),
      ("content_block_delta", #"{"index":0,"delta":{"type":"thinking_delta","thinking":"Hmm"}}"#),
      ("content_block_delta", #"{"index":0,"delta":{"type":"signature_delta","signature":"sig"}}"#),
      ("content_block_start", #"{"index":1,"content_block":{"type":"text","text":""}}"#),
      ("content_block_delta", #"{"index":1,"delta":{"type":"text_delta","text":"Looking."}}"#),
      ("content_block_start", #"{"index":2,"content_block":{"type":"tool_use","id":"toolu_1","name":"screenshot","input":{}}}"#),
      ("content_block_delta", #"{"index":2,"delta":{"type":"input_json_delta","partial_json":"{\"full\":true}"}}"#),
      ("content_block_stop", #"{"index":2}"#),
      ("message_delta", #"{"delta":{"stop_reason":"tool_use"}}"#),
    ] {
      conversation.apply(event: name, data: data)
    }
    conversation.finishReply()
    conversation.addToolResults([
      [
        "type": "tool_result", "tool_use_id": "toolu_1",
        "content": [
          ["type": "text", "text": "The page."],
          ["type": "image", "source": ["type": "base64", "media_type": "image/jpeg", "data": "QUJD"]],
        ],
      ],
      ["type": "tool_result", "tool_use_id": "toolu_2", "content": [["type": "text", "text": "No such tab."]], "is_error": true],
    ])
    let tools: [JSONValue] = [["name": "screenshot", "description": "Take one.", "input_schema": ["type": "object", "properties": [:]]]]
    return ChatRequest(
      messages: conversation.messages(),
      options: .init(model: "gpt-5", systemPrompt: "You are Claude, built into Compass.", tools: tools, claudeAPI: false)
    ).body
  }

  @Test func translatesAChatRequest() throws {
    let body = OpenAIChat.request(from: chatBody(), provider: .openAI)
    #expect(body["model"] == "gpt-5")
    #expect(body["stream"] == true)
    // Anthropic's fields are gone.
    for field in ["system", "thinking", "max_tokens", "cache_control", "fallbacks"] { #expect(body[field] == nil) }
    // The chat thinks: no light effort.
    #expect(body["reasoning_effort"] == nil)
    let messages = try #require(body["messages"]?.array)
    #expect(messages.map { $0["role"]?.string } == ["system", "user", "assistant", "tool", "tool", "user"])
    // The system prompt, joined, with a neutral name.
    let system = try #require(messages[0]["content"]?.string)
    #expect(system.hasPrefix("You are an AI assistant, built into Compass."))
    #expect(system.contains("Today is"))
    // The user's page block and text, as one string.
    let user = try #require(messages[1]["content"]?.string)
    #expect(user.contains("<page url=\"https://a.example/\""))
    #expect(user.hasSuffix("Click the button"))
    // The assistant: its text, no thinking, the call with its arguments.
    #expect(messages[2]["content"] == "Looking.")
    let call = try #require(messages[2]["tool_calls"]?.array?.first)
    #expect(call["id"] == "toolu_1")
    #expect(call["function"]?["name"] == "screenshot")
    #expect(JSONValue.parse(call["function"]?["arguments"]?.string ?? "") == ["full": true])
    // The results: text, the image's note, the error.
    #expect(messages[3]["tool_call_id"] == "toolu_1")
    #expect(messages[3]["content"] == .string("The page.\n" + OpenAIChat.imageFollows))
    #expect(messages[4]["content"] == "Error: No such tab.")
    // The screenshot after the tool messages, as a data URL.
    let parts = try #require(messages[5]["content"]?.array)
    #expect(parts.first?["text"] == .string(OpenAIChat.imagesNote))
    #expect(parts.last?["image_url"]?["url"] == "data:image/jpeg;base64,QUJD")
    // The tool as a function.
    let tool = try #require(body["tools"]?.array?.first)
    #expect(tool["type"] == "function")
    #expect(tool["function"]?["name"] == "screenshot")
    #expect(tool["function"]?["parameters"]?["type"] == "object")
  }

  @Test func lightRequestsAskForLittleReasoning() {
    let quick = ClaudeQuick.body(
      .init(feature: "f", system: "Name it.", user: "Tabs"), model: "gpt-5-mini", cacheLevel: .normal, oauthToken: false)
    let openAI = OpenAIChat.request(from: quick, provider: .openAI, jsonReply: true)
    #expect(openAI["reasoning_effort"] == "low")
    #expect(openAI["response_format"] == ["type": "json_object"])
    #expect(openAI["messages"]?.array?.map { $0["role"]?.string } == ["system", "user"])
    #expect(openAI["stream"] == nil)
    // Gemini 2.5 takes it; an older model or another server doesn't get it.
    var gemini = quick
    if case .object(var object) = gemini { object["model"] = "gemini-2.5-flash"; gemini = .object(object) }
    #expect(OpenAIChat.request(from: gemini, provider: .gemini)["reasoning_effort"] == "low")
    var old = quick
    if case .object(var object) = old { object["model"] = "gpt-4.1"; old = .object(object) }
    #expect(OpenAIChat.request(from: old, provider: .openAI)["reasoning_effort"] == nil)
    #expect(OpenAIChat.request(from: quick, provider: .compatible, jsonReply: true)["reasoning_effort"] == nil)
    #expect(OpenAIChat.request(from: quick, provider: .compatible, jsonReply: true)["response_format"] == nil)
  }

  /// Feeds `lines` (each a `data:` payload) and applies the events to a
  /// reply, as ClaudeClient and the chat do.
  private func stream(_ lines: [String], finish: Bool = true) -> (Conversation.Turn?, [String], String?) {
    var translator = OpenAIChat.StreamTranslator()
    var conversation = Conversation()
    conversation.addUser(text: "Hi")
    conversation.startReply()
    var names: [String] = []
    for line in lines {
      for event in translator.feed(line) {
        names.append(event.name)
        conversation.apply(event: event.name, data: event.data)
      }
    }
    if finish {
      for event in translator.finish() {
        names.append(event.name)
        conversation.apply(event: event.name, data: event.data)
      }
    }
    conversation.finishReply()
    return (conversation.turns.last, names, translator.error)
  }

  @Test func streamsText() {
    let (turn, names, error) = stream([
      #"{"choices":[{"index":0,"delta":{"role":"assistant","content":""}}]}"#,
      #"{"choices":[{"index":0,"delta":{"content":"Hel"}}]}"#,
      #"{"choices":[{"index":0,"delta":{"content":"lo."}}]}"#,
      #"{"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}"#,
      #"{"choices":[],"usage":{"total_tokens":5}}"#,
      "[DONE]",
    ])
    #expect(error == nil)
    #expect(names.last == "message_stop")
    #expect(turn?.content == [["type": "text", "text": "Hello."]])
    #expect(turn?.stopReason == "end_turn")
  }

  @Test func streamsReasoningThenToolCalls() {
    // OpenAI's shape: calls by index, arguments in pieces, two calls.
    let (turn, _, _) = stream([
      #"{"choices":[{"delta":{"reasoning_content":"Plan."}}]}"#,
      #"{"choices":[{"delta":{"content":"On it."}}]}"#,
      #"{"choices":[{"delta":{"tool_calls":[{"index":0,"id":"call_a","type":"function","function":{"name":"click","arguments":""}}]}}]}"#,
      #"{"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"{\"ref\":"}}]}}]}"#,
      #"{"choices":[{"delta":{"tool_calls":[{"index":1,"id":"call_b","type":"function","function":{"name":"read_page","arguments":"{}"}}]}}]}"#,
      #"{"choices":[{"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"e1\"}"}}]}}]}"#,
      #"{"choices":[{"delta":{},"finish_reason":"tool_calls"}]}"#,
      "[DONE]",
    ])
    let content = turn?.content ?? []
    #expect(content.map { $0["type"]?.string } == ["thinking", "text", "tool_use", "tool_use"])
    #expect(content[0]["thinking"] == "Plan.")
    #expect(content[2]["id"] == "call_a")
    #expect(content[2]["input"] == ["ref": "e1"])
    #expect(content[3]["name"] == "read_page")
    #expect(content[3]["input"] == .object([:]))
    #expect(turn?.stopReason == "tool_use")
  }

  @Test func geminiCallsWithoutIndexOrIDAndStop() {
    // Gemini: whole calls without an index or id, and "stop" after them.
    let (turn, _, _) = stream([
      #"{"choices":[{"delta":{"role":"assistant","tool_calls":[{"function":{"name":"find","arguments":"{\"query\":\"login\"}"},"type":"function"}]}}]}"#,
      #"{"choices":[{"delta":{"tool_calls":[{"id":"","function":{"name":"read_page","arguments":"{}"},"type":"function"}]},"finish_reason":"stop"}]}"#,
    ], finish: true)
    let content = turn?.content ?? []
    #expect(content.count == 2)
    #expect(content.map { $0["name"]?.string } == ["find", "read_page"])
    // Ids made up, different, kept for the results.
    let ids = content.compactMap { $0["id"]?.string }
    #expect(ids.count == 2 && ids[0] != ids[1] && ids.allSatisfy { $0.hasPrefix("call_") })
    #expect(content[0]["input"] == ["query": "login"])
    #expect(turn?.stopReason == "tool_use")
  }

  @Test func stopReasonsErrorsAndEarlyEnds() {
    #expect(stream([#"{"choices":[{"delta":{"content":"a"},"finish_reason":"length"}]}"#, "[DONE]"]).0?.stopReason == "max_tokens")
    #expect(stream([#"{"choices":[{"delta":{},"finish_reason":"content_filter"}]}"#, "[DONE]"]).0?.stopReason == "refusal")
    // An error chunk is the reply's error.
    #expect(stream([#"{"error":{"message":"Quota exceeded","type":"insufficient_quota"}}"#]).2 == "Quota exceeded")
    // No finish reason and no [DONE]: no message_stop (the reply ended early).
    let (_, names, _) = stream([#"{"choices":[{"delta":{"content":"Half"}}]}"#])
    #expect(!names.contains("message_stop"))
  }

  @Test func oneShotReplies() {
    func data(_ json: String) -> Data { Data(json.utf8) }
    #expect(OpenAIChat.replyText(data(#"{"choices":[{"message":{"role":"assistant","content":"{\"name\":\"Rust\"}"},"finish_reason":"stop"}]}"#)) == .text(#"{"name":"Rust"}"#))
    #expect(OpenAIChat.replyText(data(#"{"choices":[{"message":{"content":null,"refusal":"No."}}]}"#)) == .refused)
    #expect(OpenAIChat.replyText(data(#"{"error":{"message":"bad"}}"#)) == .failed)
    #expect(
      ClaudeQuick.parseOpenAIResponse(data(#"{"choices":[{"message":{"content":"{\"name\":\"Rust\"}"}}]}"#), status: 200)
        == .success(["name": "Rust"]))
    #expect(ClaudeQuick.parseOpenAIResponse(data(#"{"choices":[{"message":{"content":"Rust"}}]}"#), status: 200) == .failure(.badReply))
  }

  @Test func endpointsErrorsAndModels() throws {
    let openAI = ClaudeEndpoint(provider: .openAI, baseURL: AIProvider.openAI.defaultBaseURL!, apiKey: "sk-test")
    let request = openAI.request(openAI.chatPath, body: Data("{}".utf8))
    // OpenAI's own models take tools only through the Responses API.
    #expect(request.url?.absoluteString == "https://api.openai.com/v1/responses")
    #expect(request.value(forHTTPHeaderField: "Authorization") == "Bearer sk-test")
    #expect(request.value(forHTTPHeaderField: "anthropic-version") == nil)
    #expect(request.value(forHTTPHeaderField: "x-api-key") == nil)
    #expect(!openAI.isClaudeAPI && !openAI.usesOAuthToken)
    #expect(openAI.url(openAI.modelsPath).absoluteString == "https://api.openai.com/v1/models")
    let gemini = try #require(ClaudeEndpoint.parseBaseURL("", provider: .gemini))
    #expect(gemini.absoluteString == "https://generativelanguage.googleapis.com/v1beta/openai")
    #expect(ClaudeEndpoint.parseBaseURL("", provider: .compatible) == nil)
    #expect(ClaudeEndpoint.parseBaseURL("http://localhost:11434/v1", provider: .compatible) != nil)
    // A local server without a key: no Authorization.
    let local = ClaudeEndpoint(provider: .compatible, baseURL: URL(string: "http://localhost:11434/v1")!, apiKey: "")
    #expect(local.request("models").value(forHTTPHeaderField: "Authorization") == nil)
    // Errors: Gemini's array, and words for the provider.
    #expect(ClaudeEndpoint.describeError(status: 400, body: Data(#"[{"error":{"code":400,"message":"API key not valid."}}]"#.utf8), provider: .gemini) == "API key not valid.")
    #expect(ClaudeEndpoint.describeError(status: 529, body: Data(), provider: .openAI).hasPrefix("The server is overloaded"))
    #expect(ClaudeEndpoint.describeError(status: 529, body: Data()).hasPrefix("Claude is overloaded"))
    // Model lists: OpenAI's chat models only, Gemini's without models/.
    let openAIModels = ClaudeEndpoint.parseModels(
      Data(#"{"data":[{"id":"gpt-5"},{"id":"text-embedding-3-large"},{"id":"whisper-1"},{"id":"gpt-5-mini"},{"id":"gpt-image-1"}]}"#.utf8),
      provider: .openAI)
    #expect(openAIModels?.map(\.id) == ["gpt-5", "gpt-5-mini"])
    let geminiModels = ClaudeEndpoint.parseModels(
      Data(#"{"object":"list","data":[{"id":"models/gemini-2.5-flash","object":"model"}]}"#.utf8), provider: .gemini)
    #expect(geminiModels?.map(\.id) == ["gemini-2.5-flash"])
    // Anthropic's own requests don't change.
    let claude = ClaudeEndpoint(apiKey: "sk-ant-api03-key")
    let claudeRequest = claude.request(claude.chatPath, body: Data("{}".utf8))
    #expect(claudeRequest.url?.absoluteString == "https://api.anthropic.com/v1/messages")
    #expect(claudeRequest.value(forHTTPHeaderField: "x-api-key") == "sk-ant-api03-key")
    #expect(claudeRequest.value(forHTTPHeaderField: "anthropic-version") == ClaudeEndpoint.apiVersion)
  }

  /// The sidebar's requests on Gemini, through a fake Chat Completions
  /// server: the path, the key, the body, and the reply's JSON.
  @MainActor @Test func sidebarRequestsOnGemini() async throws {
    actor Server {
      var requests: [URLRequest] = []
      func handle(_ request: URLRequest) -> (Data, Int) {
        requests.append(request)
        let reply: JSONValue = [
          "choices": [["index": 0, "message": ["role": "assistant", "content": #"{"name": "Rust Async"}"#], "finish_reason": "stop"]]
        ]
        return (reply.encoded(), 200)
      }
    }
    let server = Server()
    let quick = ClaudeQuick(
      endpoint: { ClaudeEndpoint(provider: .gemini, baseURL: AIProvider.gemini.defaultBaseURL!, apiKey: "AIza-test") },
      isOn: { _ in true },
      transport: { request in await server.handle(request) })
    let result = await quick.ask(.init(feature: ClaudeQuick.nameLinkFoldersKey, system: "Name it.", user: "- A"))
    #expect(result == .success(["name": "Rust Async"]))
    let request = try #require(await server.requests.first)
    #expect(request.url?.absoluteString == "https://generativelanguage.googleapis.com/v1beta/openai/chat/completions")
    #expect(request.value(forHTTPHeaderField: "Authorization") == "Bearer AIza-test")
    let body = try #require(request.httpBody.flatMap(JSONValue.parse))
    // An empty model is the provider's sidebar default.
    #expect(body["model"] == .string(AIProvider.gemini.defaultQuickModel))
    #expect(body["response_format"] == ["type": "json_object"])
    #expect(body["max_tokens"] == nil && body["cache_control"] == nil)
  }
}
