// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassClaude

/// docs/design/ai-providers.md, "OpenAI: the Responses API".
struct OpenAIResponsesTests {
  private func body() -> JSONValue {
    let messages: [JSONValue] = [
      ["role": "user", "content": [["type": "text", "text": "Click it"]]],
      [
        "role": "assistant",
        "content": [
          ["type": "thinking", "thinking": "Plan", "signature": "sig"],
          ["type": "text", "text": "Looking."],
          ["type": "tool_use", "id": "call_1", "name": "screenshot", "input": ["full": true]],
        ],
      ],
      [
        "role": "user",
        "content": [
          [
            "type": "tool_result", "tool_use_id": "call_1",
            "content": [
              ["type": "text", "text": "Shot."],
              ["type": "image", "source": ["type": "base64", "media_type": "image/jpeg", "data": "QUJD"]],
            ],
          ],
          ["type": "text", "text": "And now?"],
        ],
      ],
    ]
    return [
      "model": "gpt-6.1-terra", "max_tokens": 64000, "stream": true,
      "thinking": ["type": "adaptive"], "cache_control": ["type": "ephemeral"],
      "system": [["type": "text", "text": "You are Claude, built into Compass."]],
      "messages": .array(messages),
      "tools": [["name": "screenshot", "description": "Take one.", "input_schema": ["type": "object", "properties": [:]]]],
    ]
  }

  @Test func translatesAChatRequest() throws {
    let out = OpenAIResponses.request(from: body())
    #expect(out["model"] == "gpt-6.1-terra" && out["stream"] == true && out["store"] == false)
    #expect(out["instructions"] == "You are an AI assistant, built into Compass.")
    for field in ["system", "messages", "thinking", "max_tokens", "cache_control", "reasoning"] { #expect(out[field] == nil) }
    let input = try #require(out["input"]?.array)
    #expect(input.count == 5)
    #expect(input[0] == ["role": "user", "content": "Click it"])
    #expect(input[1] == ["role": "assistant", "content": "Looking."])
    #expect(input[2]["type"] == "function_call" && input[2]["call_id"] == "call_1" && input[2]["name"] == "screenshot")
    #expect(JSONValue.parse(input[2]["arguments"]?.string ?? "") == ["full": true])
    #expect(input[3]["type"] == "function_call_output")
    #expect(input[3]["output"] == .string("Shot.\n" + OpenAIChat.imageFollows))
    let parts = try #require(input[4]["content"]?.array)
    #expect(parts.map { $0["type"]?.string } == ["input_text", "input_image", "input_text"])
    #expect(parts[1]["image_url"] == "data:image/jpeg;base64,QUJD")
    #expect(parts[2]["text"] == "And now?")
    let tool = try #require(out["tools"]?.array?.first)
    #expect(tool == ["type": "function", "name": "screenshot", "description": "Take one.", "parameters": ["type": "object", "properties": [:]]])
  }

  @Test func lightRequests() {
    let quick = ClaudeQuick.body(
      .init(feature: "f", system: "Name it.", user: "Tabs"), model: "gpt-6-luna", cacheLevel: .normal, oauthToken: false)
    let out = OpenAIResponses.request(from: quick, jsonReply: true)
    #expect(out["reasoning"] == ["effort": "low"])
    #expect(out["text"] == ["format": ["type": "json_object"]])
    #expect(out["instructions"] == "Name it.")
    #expect(out["stream"] == nil)
  }

  private func stream(_ events: [String]) -> (Conversation.Turn?, String?) {
    var translator = OpenAIEdge.Stream(provider: .openAI)
    var conversation = Conversation()
    conversation.addUser(text: "Hi")
    conversation.startReply()
    for data in events {
      for event in translator.feed(data) { conversation.apply(event: event.name, data: event.data) }
    }
    for event in translator.finish() { conversation.apply(event: event.name, data: event.data) }
    conversation.finishReply()
    return (conversation.turns.last, translator.error)
  }

  @Test func streamsReasoningTextAndCalls() {
    let (turn, error) = stream([
      #"{"type":"response.created","response":{"id":"resp_1","status":"in_progress"}}"#,
      #"{"type":"response.output_item.added","output_index":0,"item":{"type":"reasoning","id":"rs_1"}}"#,
      #"{"type":"response.reasoning_summary_text.delta","output_index":0,"summary_index":0,"delta":"Plan."}"#,
      #"{"type":"response.output_item.added","output_index":1,"item":{"type":"message","id":"msg_1","role":"assistant"}}"#,
      #"{"type":"response.output_text.delta","output_index":1,"content_index":0,"delta":"On "}"#,
      #"{"type":"response.output_text.delta","output_index":1,"content_index":0,"delta":"it."}"#,
      #"{"type":"response.output_item.added","output_index":2,"item":{"type":"function_call","id":"fc_1","call_id":"call_a","name":"click","arguments":""}}"#,
      #"{"type":"response.function_call_arguments.delta","output_index":2,"delta":"{\"ref\":"}"#,
      #"{"type":"response.function_call_arguments.delta","output_index":2,"delta":"\"e1\"}"}"#,
      #"{"type":"response.output_item.done","output_index":2,"item":{"type":"function_call","call_id":"call_a","name":"click","arguments":"{\"ref\":\"e1\"}"}}"#,
      // A call whose arguments come only whole.
      #"{"type":"response.output_item.added","output_index":3,"item":{"type":"function_call","call_id":"call_b","name":"read_page","arguments":""}}"#,
      #"{"type":"response.output_item.done","output_index":3,"item":{"type":"function_call","call_id":"call_b","name":"read_page","arguments":"{\"filter\":\"interactive\"}"}}"#,
      #"{"type":"response.completed","response":{"id":"resp_1","status":"completed"}}"#,
    ])
    #expect(error == nil)
    let content = turn?.content ?? []
    #expect(content.map { $0["type"]?.string } == ["thinking", "text", "tool_use", "tool_use"])
    #expect(content[0]["thinking"] == "Plan.")
    #expect(content[1]["text"] == "On it.")
    // Arguments once, not twice (deltas and the item's own).
    #expect(content[2]["id"] == "call_a" && content[2]["input"] == ["ref": "e1"])
    #expect(content[3]["id"] == "call_b" && content[3]["input"] == ["filter": "interactive"])
    #expect(turn?.stopReason == "tool_use")
  }

  @Test func endsAndFailures() {
    #expect(stream([
      #"{"type":"response.output_text.delta","output_index":0,"delta":"Hi"}"#,
      #"{"type":"response.completed","response":{"status":"completed"}}"#,
    ]).0?.stopReason == "end_turn")
    #expect(stream([
      #"{"type":"response.output_text.delta","output_index":0,"delta":"Hi"}"#,
      #"{"type":"response.incomplete","response":{"incomplete_details":{"reason":"max_output_tokens"}}}"#,
    ]).0?.stopReason == "max_tokens")
    #expect(stream([#"{"type":"response.failed","response":{"error":{"code":"server_error","message":"Boom"}}}"#]).1 == "Boom")
    #expect(stream([#"{"type":"error","code":"rate_limit_exceeded","message":"Slow down"}"#]).1 == "Slow down")
    // No response.completed: no stop (the reply ended early).
    #expect(stream([#"{"type":"response.output_text.delta","output_index":0,"delta":"Half"}"#]).0?.stopReason == nil)
  }

  @Test func oneShotReplies() {
    func data(_ json: String) -> Data { Data(json.utf8) }
    #expect(OpenAIResponses.replyText(data(#"{"status":"completed","error":null,"output":[{"type":"reasoning","summary":[]},{"type":"message","content":[{"type":"output_text","text":"{\"name\":\"Rust\"}"}]}]}"#)) == .text(#"{"name":"Rust"}"#))
    #expect(OpenAIResponses.replyText(data(#"{"output":[{"type":"message","content":[{"type":"refusal","refusal":"No."}]}]}"#)) == .refused)
    #expect(OpenAIResponses.replyText(data(#"{"error":{"message":"bad"}}"#)) == .failed)
    #expect(
      ClaudeQuick.parseOpenAIResponse(
        data(#"{"output":[{"type":"message","content":[{"type":"output_text","text":"{\"name\":\"Rust\"}"}]}]}"#), status: 200,
        provider: .openAI) == .success(["name": "Rust"]))
  }

  /// The sidebar's requests on OpenAI: the Responses path, Luna by
  /// default, little reasoning, a JSON reply.
  @MainActor @Test func sidebarRequestsOnOpenAI() async throws {
    actor Server {
      var requests: [URLRequest] = []
      func handle(_ request: URLRequest) -> (Data, Int) {
        requests.append(request)
        let reply: JSONValue = [
          "status": "completed",
          "output": [["type": "message", "content": [["type": "output_text", "text": #"{"name": "Rust Async"}"#]]]],
        ]
        return (reply.encoded(), 200)
      }
    }
    let server = Server()
    let quick = ClaudeQuick(
      endpoint: { ClaudeEndpoint(provider: .openAI, baseURL: AIProvider.openAI.defaultBaseURL!, apiKey: "sk-test") },
      isOn: { _ in true },
      transport: { request in await server.handle(request) })
    let result = await quick.ask(.init(feature: ClaudeQuick.nameLinkFoldersKey, system: "Name it.", user: "- A"))
    #expect(result == .success(["name": "Rust Async"]))
    let request = try #require(await server.requests.first)
    #expect(request.url?.absoluteString == "https://api.openai.com/v1/responses")
    let body = try #require(request.httpBody.flatMap(JSONValue.parse))
    #expect(body["model"] == "gpt-6-luna")
    #expect(body["reasoning"] == ["effort": "low"])
    #expect(body["text"] == ["format": ["type": "json_object"]])
  }
}
