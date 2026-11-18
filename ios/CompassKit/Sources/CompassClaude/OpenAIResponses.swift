// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// OpenAI's Responses API, which OpenAI's own models need for tools (GPT-6
/// calls tools only there), translated to and from the Messages API as
/// `OpenAIChat` does for Chat Completions (docs/design/ai-providers.md,
/// "OpenAI: the Responses API"). Stateless (`store: false`): every request
/// carries the whole conversation, as with Claude.
public enum OpenAIResponses {
  // MARK: The request

  /// A Messages API body as a Responses body. `jsonReply`: the reply must
  /// be one JSON object. A body without `thinking` is a light request,
  /// which asks for little reasoning.
  public static func request(from body: JSONValue, jsonReply: Bool = false) -> JSONValue {
    let model = body["model"]?.string ?? ""
    var input: [JSONValue] = []
    for message in body["messages"]?.array ?? [] {
      input += convert(message)
    }
    var out: [String: JSONValue] = ["model": .string(model), "input": .array(input), "store": false]
    let instructions = OpenAIChat.systemText(body["system"])
    if !instructions.isEmpty { out["instructions"] = .string(instructions) }
    if body["stream"] == true { out["stream"] = true }
    let tools: [JSONValue] = (body["tools"]?.array ?? []).compactMap { definition in
      guard let name = definition["name"]?.string else { return nil }
      var tool: [String: JSONValue] = ["type": "function", "name": .string(name)]
      if let description = definition["description"]?.string { tool["description"] = .string(description) }
      tool["parameters"] = definition["input_schema"] ?? ["type": "object", "properties": .object([:])]
      return .object(tool)
    }
    if !tools.isEmpty { out["tools"] = .array(tools) }
    if body["thinking"] == nil, let effort = OpenAIChat.lightEffort(model: model, provider: .openAI) {
      out["reasoning"] = ["effort": .string(effort)]
    }
    if jsonReply { out["text"] = ["format": ["type": "json_object"]] }
    return .object(out)
  }

  /// One Messages API message as Responses input items: an assistant's text
  /// and its function calls; or a user message's tool results as function
  /// call outputs, then its text and images (the results' too) as a user
  /// message.
  static func convert(_ message: JSONValue) -> [JSONValue] {
    let role = message["role"]?.string ?? "user"
    let blocks: [JSONValue] =
      message["content"]?.array ?? (message["content"]?.string.map { [["type": "text", "text": .string($0)]] } ?? [])
    var items: [JSONValue] = []
    if role == "assistant" {
      let text = blocks.filter { $0["type"]?.string == "text" }.compactMap { $0["text"]?.string }.joined()
      if !text.isEmpty { items.append(["role": "assistant", "content": .string(text)]) }
      for block in blocks where block["type"]?.string == "tool_use" {
        items.append([
          "type": "function_call", "call_id": block["id"] ?? "", "name": block["name"] ?? "",
          "arguments": .string(String(decoding: (block["input"] ?? .object([:])).encoded(), as: UTF8.self)),
        ])
      }
      return items
    }
    var parts: [JSONValue] = []
    var resultImages: [JSONValue] = []
    for block in blocks {
      switch block["type"]?.string {
      case "tool_result":
        let inner = block["content"]?.array ?? (block["content"]?.string.map { [["type": "text", "text": .string($0)]] } ?? [])
        var text = inner.compactMap { $0["type"]?.string == "text" ? $0["text"]?.string : nil }.joined(separator: "\n")
        let images = inner.compactMap(imagePart)
        if !images.isEmpty {
          text += (text.isEmpty ? "" : "\n") + OpenAIChat.imageFollows
          resultImages += images
        }
        if block["is_error"] == true { text = "Error: " + text }
        items.append(["type": "function_call_output", "call_id": block["tool_use_id"] ?? "", "output": .string(text)])
      case "text":
        if let text = block["text"]?.string { parts.append(["type": "input_text", "text": .string(text)]) }
      case "image":
        if let image = imagePart(block) { parts.append(image) }
      default:
        break
      }
    }
    if !resultImages.isEmpty { parts = [["type": "input_text", "text": .string(OpenAIChat.imagesNote)]] + resultImages + parts }
    if !parts.isEmpty {
      if parts.allSatisfy({ $0["type"]?.string == "input_text" }) {
        let text = parts.compactMap { $0["text"]?.string }.joined(separator: "\n\n")
        items.append(["role": "user", "content": .string(text)])
      } else {
        items.append(["role": "user", "content": .array(parts)])
      }
    }
    return items
  }

  /// A base64 image block as an `input_image` part with a data URL.
  static func imagePart(_ block: JSONValue) -> JSONValue? {
    guard let part = OpenAIChat.imagePart(block), let url = part["image_url"]?["url"] else { return nil }
    return ["type": "input_image", "image_url": url]
  }

  // MARK: The one-shot reply

  /// A Responses response's text, or why there's none.
  public static func replyText(_ data: Data) -> OpenAIChat.Reply {
    guard let response = JSONValue.parse(data), response["error"] == nil || response["error"] == .null else {
      return .failed
    }
    if response["status"]?.string == "failed" { return .failed }
    if response["incomplete_details"]?["reason"]?.string == "content_filter" { return .refused }
    var text = ""
    for item in response["output"]?.array ?? [] where item["type"]?.string == "message" {
      for part in item["content"]?.array ?? [] {
        switch part["type"]?.string {
        case "output_text": text += part["text"]?.string ?? ""
        case "refusal": return .refused
        default: break
        }
      }
    }
    return .text(text)
  }

  // MARK: The stream

  /// Turns a Responses stream's events into the Messages API's, as
  /// `OpenAIChat.StreamTranslator` does for Chat Completions.
  public struct StreamTranslator: Sendable {
    public typealias Event = OpenAIChat.StreamTranslator.Event

    /// Blocks by the output item they came from (`output_index`, and the
    /// part: text and reasoning of one item are separate blocks).
    private var blocks: [String: Int] = [:]
    /// Function calls' blocks, and whether their arguments came as deltas.
    private var calls: [Int: (block: Int, streamed: Bool)] = [:]
    private var order: [Int] = []
    private var nextIndex = 0
    private var ended = false
    public private(set) var error: String?

    public init() {}

    /// The events for one `data:` payload (its `type` says what it is).
    public mutating func feed(_ data: String) -> [Event] {
      guard !ended, let event = JSONValue.parse(data) else { return [] }
      let output = event["output_index"]?.number.map(Int.init) ?? 0
      switch event["type"]?.string {
      case "response.output_item.added":
        guard let item = event["item"], item["type"]?.string == "function_call" else { return [] }
        let index = open("call\(output)", ["type": "tool_use", "id": callID(item, output), "name": item["name"] ?? "", "input": .object([:])])
        calls[output] = (index.block, false)
        return index.events
      case "response.output_text.delta", "response.refusal.delta":
        let opened = open("text\(output)", ["type": "text", "text": ""])
        return opened.events + [delta(opened.block, ["type": "text_delta", "text": event["delta"] ?? ""])]
      case "response.reasoning_summary_text.delta", "response.reasoning_text.delta":
        let opened = open("thinking\(output)", ["type": "thinking", "thinking": ""])
        return opened.events + [delta(opened.block, ["type": "thinking_delta", "thinking": event["delta"] ?? ""])]
      case "response.function_call_arguments.delta":
        guard let call = calls[output] else { return [] }
        calls[output] = (call.block, true)
        return [delta(call.block, ["type": "input_json_delta", "partial_json": event["delta"] ?? ""])]
      case "response.output_item.done":
        // A call whose arguments came whole.
        guard let item = event["item"], item["type"]?.string == "function_call" else { return [] }
        var events: [Event] = []
        if calls[output] == nil {
          let opened = open("call\(output)", ["type": "tool_use", "id": callID(item, output), "name": item["name"] ?? "", "input": .object([:])])
          calls[output] = (opened.block, false)
          events += opened.events
        }
        if let call = calls[output], !call.streamed, let arguments = item["arguments"]?.string, !arguments.isEmpty {
          calls[output] = (call.block, true)
          events.append(delta(call.block, ["type": "input_json_delta", "partial_json": .string(arguments)]))
        }
        return events
      case "response.completed":
        return end("end_turn")
      case "response.incomplete":
        let reason = event["response"]?["incomplete_details"]?["reason"]?.string
        return end(reason == "content_filter" ? "refusal" : "max_tokens")
      case "response.failed":
        error = event["response"]?["error"]?["message"]?.string ?? "The reply failed."
        ended = true
        return []
      case "error":
        error = event["message"]?.string ?? event["error"]?["message"]?.string ?? "The reply failed."
        ended = true
        return []
      default:
        return []
      }
    }

    private func callID(_ item: JSONValue, _ output: Int) -> JSONValue {
      if let id = item["call_id"]?.string, !id.isEmpty { return .string(id) }
      return .string("call_\(output)")
    }

    /// The block for `key`, started if new.
    private mutating func open(_ key: String, _ block: JSONValue) -> (block: Int, events: [Event]) {
      if let known = blocks[key] { return (known, []) }
      let index = nextIndex
      nextIndex += 1
      blocks[key] = index
      order.append(index)
      return (index, [event("content_block_start", ["type": "content_block_start", "index": .number(Double(index)), "content_block": block])])
    }

    private func delta(_ index: Int, _ delta: JSONValue) -> Event {
      event("content_block_delta", ["type": "content_block_delta", "index": .number(Double(index)), "delta": delta])
    }

    private mutating func end(_ reason: String) -> [Event] {
      ended = true
      var events = order.map { event("content_block_stop", ["type": "content_block_stop", "index": .number(Double($0))]) }
      let stop = calls.isEmpty || reason != "end_turn" ? reason : "tool_use"
      events.append(event("message_delta", ["type": "message_delta", "delta": ["stop_reason": .string(stop)]]))
      events.append(event("message_stop", ["type": "message_stop"]))
      return events
    }

    private func event(_ name: String, _ data: JSONValue) -> Event {
      (name, String(decoding: data.encoded(), as: UTF8.self))
    }
  }
}

extension AIProvider {
  /// OpenAI's own models take tools only through the Responses API; the
  /// others speak Chat Completions.
  public var usesResponsesAPI: Bool { self == .openAI }
}

/// The translation a provider other than Anthropic gets: the Responses API
/// for OpenAI, Chat Completions for the others.
public enum OpenAIEdge {
  public static func request(from body: JSONValue, provider: AIProvider, jsonReply: Bool = false) -> JSONValue {
    provider.usesResponsesAPI
      ? OpenAIResponses.request(from: body, jsonReply: jsonReply)
      : OpenAIChat.request(from: body, provider: provider, jsonReply: jsonReply)
  }

  public static func replyText(_ data: Data, provider: AIProvider) -> OpenAIChat.Reply {
    provider.usesResponsesAPI ? OpenAIResponses.replyText(data) : OpenAIChat.replyText(data)
  }

  /// A streamed reply's translator for `provider`.
  public struct Stream: Sendable {
    private var chat = OpenAIChat.StreamTranslator()
    private var responses = OpenAIResponses.StreamTranslator()
    private let usesResponses: Bool

    public init(provider: AIProvider) { usesResponses = provider.usesResponsesAPI }

    /// The Messages events for one `data:` payload.
    public mutating func feed(_ data: String) -> [OpenAIChat.StreamTranslator.Event] {
      usesResponses ? responses.feed(data) : chat.feed(data)
    }

    /// The closing events when the stream ends (none if it ended early).
    public mutating func finish() -> [OpenAIChat.StreamTranslator.Event] {
      usesResponses ? [] : chat.finish()
    }

    public var error: String? { usesResponses ? responses.error : chat.error }
  }
}
