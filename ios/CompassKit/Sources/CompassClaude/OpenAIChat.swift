// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// OpenAI's Chat Completions, which OpenAI, Gemini and most other servers
/// speak, translated to and from the Messages API Compass builds and reads
/// (docs/design/ai-providers.md). Requests are built as for Claude, then
/// `request(from:)` turns them into Chat Completions; a streamed reply's
/// chunks become the Messages events they stand for (`StreamTranslator`),
/// and a one-shot reply its text (`replyText`), so nothing past the edge
/// knows which provider answered.
public enum OpenAIChat {
  /// The system prompt's first words, and what they become.
  static let claudeIdentity = "You are Claude,"
  static let neutralIdentity = "You are an AI assistant,"

  /// What a tool message says when its result had images, which go in a
  /// user message after the tool messages (Chat Completions takes no image
  /// in a tool message).
  static let imageFollows = "(The screenshot follows.)"
  static let imagesNote = "The screenshots from the tool results above:"

  // MARK: The request

  /// A Messages API body as a Chat Completions body for `provider`.
  /// `jsonReply`: the reply must be one JSON object (the sidebar's
  /// requests). A body without `thinking` is a light request (the
  /// sidebar's, the command bar's), which asks for little reasoning where
  /// the provider knows the field.
  public static func request(from body: JSONValue, provider: AIProvider, jsonReply: Bool = false) -> JSONValue {
    let model = body["model"]?.string ?? ""
    var messages: [JSONValue] = []
    let system = systemText(body["system"])
    if !system.isEmpty { messages.append(["role": "system", "content": .string(system)]) }
    for message in body["messages"]?.array ?? [] {
      messages += convert(message)
    }
    var out: [String: JSONValue] = ["model": .string(model), "messages": .array(messages)]
    if body["stream"] == true { out["stream"] = true }
    let tools = (body["tools"]?.array ?? []).compactMap(tool)
    if !tools.isEmpty { out["tools"] = .array(tools) }
    if body["thinking"] == nil, let effort = lightEffort(model: model, provider: provider) {
      out["reasoning_effort"] = .string(effort)
    }
    if jsonReply, provider == .openAI || provider == .gemini {
      out["response_format"] = ["type": "json_object"]
    }
    return .object(out)
  }

  /// The system blocks' text, joined; Claude's name made neutral.
  static func systemText(_ system: JSONValue?) -> String {
    var text: String
    if let string = system?.string {
      text = string
    } else {
      text = (system?.array ?? []).compactMap { $0["text"]?.string }.filter { !$0.isEmpty }.joined(separator: "\n\n")
    }
    if text.hasPrefix(claudeIdentity) { text = neutralIdentity + text.dropFirst(claudeIdentity.count) }
    return text
  }

  /// "low" for the reasoning models that take it (OpenAI's gpt-5, gpt-6
  /// and o series, Gemini 2.5 and later); nil for others, which may refuse it.
  static func lightEffort(model: String, provider: AIProvider) -> String? {
    let id = model.lowercased().replacingOccurrences(of: "models/", with: "")
    switch provider {
    case .openAI:
      let reasoning = id.hasPrefix("gpt-5") || id.hasPrefix("gpt-6") || (id.hasPrefix("o") && id.dropFirst().first?.isNumber == true)
      return reasoning ? "low" : nil
    case .gemini:
      return id.hasPrefix("gemini-2.5") || id.hasPrefix("gemini-3") ? "low" : nil
    case .anthropic, .compatible:
      return nil
    }
  }

  /// A tools.json definition as a function tool.
  static func tool(_ definition: JSONValue) -> JSONValue? {
    guard let name = definition["name"]?.string else { return nil }
    var function: [String: JSONValue] = ["name": .string(name)]
    if let description = definition["description"]?.string { function["description"] = .string(description) }
    function["parameters"] = definition["input_schema"] ?? ["type": "object", "properties": .object([:])]
    return ["type": "function", "function": .object(function)]
  }

  /// One Messages API message as Chat Completions messages: an assistant
  /// message with its tool calls; or a user message's tool results as tool
  /// messages, then its text and images (the results' images too) as a
  /// user message.
  static func convert(_ message: JSONValue) -> [JSONValue] {
    let role = message["role"]?.string ?? "user"
    let blocks: [JSONValue] =
      message["content"]?.array ?? (message["content"]?.string.map { [["type": "text", "text": .string($0)]] } ?? [])
    if role == "assistant" {
      let text = blocks.filter { $0["type"]?.string == "text" }.compactMap { $0["text"]?.string }.joined()
      let calls: [JSONValue] = blocks.filter { $0["type"]?.string == "tool_use" }.map { block in
        let arguments = String(decoding: (block["input"] ?? .object([:])).encoded(), as: UTF8.self)
        return [
          "id": block["id"] ?? "", "type": "function",
          "function": ["name": block["name"] ?? "", "arguments": .string(arguments)],
        ]
      }
      guard !text.isEmpty || !calls.isEmpty else { return [] }
      var out: [String: JSONValue] = ["role": "assistant", "content": text.isEmpty ? .null : .string(text)]
      if !calls.isEmpty { out["tool_calls"] = .array(calls) }
      return [.object(out)]
    }
    var out: [JSONValue] = []
    var parts: [JSONValue] = []
    var resultImages: [JSONValue] = []
    for block in blocks {
      switch block["type"]?.string {
      case "tool_result":
        let inner = block["content"]?.array ?? (block["content"]?.string.map { [["type": "text", "text": .string($0)]] } ?? [])
        var text = inner.compactMap { $0["type"]?.string == "text" ? $0["text"]?.string : nil }.joined(separator: "\n")
        let images = inner.compactMap(imagePart)
        if !images.isEmpty {
          text += (text.isEmpty ? "" : "\n") + imageFollows
          resultImages += images
        }
        if block["is_error"] == true { text = "Error: " + text }
        out.append(["role": "tool", "tool_call_id": block["tool_use_id"] ?? "", "content": .string(text)])
      case "text":
        if let text = block["text"]?.string { parts.append(["type": "text", "text": .string(text)]) }
      case "image":
        if let image = imagePart(block) { parts.append(image) }
      default:
        break
      }
    }
    if !resultImages.isEmpty { parts = [["type": "text", "text": .string(imagesNote)]] + resultImages + parts }
    if !parts.isEmpty {
      // Plain text as a string: every server takes that.
      if parts.allSatisfy({ $0["type"]?.string == "text" }) {
        let text = parts.compactMap { $0["text"]?.string }.joined(separator: "\n\n")
        out.append(["role": "user", "content": .string(text)])
      } else {
        out.append(["role": "user", "content": .array(parts)])
      }
    }
    return out
  }

  /// A base64 image block as an `image_url` part with a data URL.
  static func imagePart(_ block: JSONValue) -> JSONValue? {
    guard block["type"]?.string == "image", let source = block["source"], source["type"]?.string == "base64",
      let data = source["data"]?.string
    else { return nil }
    let type = source["media_type"]?.string ?? "image/jpeg"
    return ["type": "image_url", "image_url": ["url": .string("data:\(type);base64,\(data)")]]
  }

  // MARK: The one-shot reply

  /// A Chat Completions response's text, or why there's none.
  public enum Reply: Equatable, Sendable {
    case text(String)
    case refused
    case failed
  }

  public static func replyText(_ data: Data) -> Reply {
    guard let response = JSONValue.parse(data), response["error"] == nil,
      let choice = response["choices"]?.array?.first
    else { return .failed }
    let message = choice["message"]
    if let refusal = message?["refusal"]?.string, !refusal.isEmpty { return .refused }
    if choice["finish_reason"]?.string == "content_filter" { return .refused }
    if let text = message?["content"]?.string { return .text(text) }
    // Some servers send parts.
    let parts = message?["content"]?.array ?? []
    return .text(parts.compactMap { $0["text"]?.string }.joined())
  }

  // MARK: Models

  /// `GET models`' list for `provider`: OpenAI's cut to chat models,
  /// Gemini's ids without `models/`. Nil if it isn't a list.
  public static func parseModels(_ data: Data, provider: AIProvider) -> [ClaudeModel]? {
    guard let list = JSONValue.parse(data)?["data"]?.array else { return nil }
    let notChat = ["embedding", "tts", "whisper", "dall-e", "moderation", "image", "audio", "realtime", "transcribe", "search", "davinci", "babbage", "sora"]
    return list.compactMap { item in
      guard var id = item["id"]?.string, !id.isEmpty else { return nil }
      if id.hasPrefix("models/") { id = String(id.dropFirst("models/".count)) }
      if provider == .openAI, notChat.contains(where: { id.lowercased().contains($0) }) { return nil }
      let name = item["display_name"]?.string ?? ""
      return ClaudeModel(id: id, displayName: name.isEmpty ? id : name)
    }
    .sorted { $0.id < $1.id }
  }

  // MARK: The stream

  /// Turns a Chat Completions stream's chunks into the Messages API's
  /// events, as (event name, JSON data) pairs that `Conversation.apply`
  /// and the command bar read.
  public struct StreamTranslator: Sendable {
    public typealias Event = (name: String, data: String)

    /// The block being written to, and its kind ("text", "thinking").
    private var open: (index: Int, kind: String)?
    /// Tool calls' blocks, by the call's index (or id).
    private var tools: [String: Int] = [:]
    private var toolOrder: [Int] = []
    private var nextIndex = 0
    private var finishReason: String?
    private var ended = false
    /// What went wrong, from an error chunk.
    public private(set) var error: String?

    public init() {}

    /// The events for one `data:` line.
    public mutating func feed(_ data: String) -> [Event] {
      guard !ended else { return [] }
      let trimmed = data.trimmingCharacters(in: .whitespacesAndNewlines)
      if trimmed == "[DONE]" { return end() }
      guard let chunk = JSONValue.parse(trimmed) else { return [] }
      if let failure = chunk["error"] {
        error = failure["message"]?.string ?? failure.string ?? "The reply failed."
        ended = true
        return []
      }
      guard let choice = chunk["choices"]?.array?.first else { return [] }
      var events: [Event] = []
      let delta = choice["delta"]
      if let reasoning = delta?["reasoning_content"]?.string ?? delta?["reasoning"]?.string, !reasoning.isEmpty {
        events += write("thinking", ["type": "thinking_delta", "thinking": .string(reasoning)])
      }
      if let text = delta?["content"]?.string, !text.isEmpty {
        events += write("text", ["type": "text_delta", "text": .string(text)])
      }
      for call in delta?["tool_calls"]?.array ?? [] {
        events += toolDelta(call)
      }
      if let reason = choice["finish_reason"]?.string, !reason.isEmpty { finishReason = reason }
      return events
    }

    /// The stream ended: the closing events if it had a finish reason,
    /// else none (the reply ended early).
    public mutating func finish() -> [Event] {
      guard !ended, finishReason != nil else { return [] }
      return end()
    }

    /// Text or thinking: into the open block of that kind, or a new one.
    private mutating func write(_ kind: String, _ delta: JSONValue) -> [Event] {
      var events: [Event] = []
      if open?.kind != kind {
        events += closeOpen()
        let index = nextIndex
        nextIndex += 1
        open = (index, kind)
        let block: JSONValue = kind == "text" ? ["type": "text", "text": ""] : ["type": "thinking", "thinking": ""]
        events.append(event("content_block_start", ["type": "content_block_start", "index": .number(Double(index)), "content_block": block]))
      }
      events.append(event("content_block_delta", ["type": "content_block_delta", "index": .number(Double(open!.index)), "delta": delta]))
      return events
    }

    private mutating func toolDelta(_ call: JSONValue) -> [Event] {
      var events: [Event] = []
      // The call's index, else its id, else a new call per chunk (Gemini
      // can send whole calls without either).
      let key = call["index"]?.number.map { "i\(Int($0))" } ?? call["id"]?.string.map { "d" + $0 } ?? "n\(nextIndex)"
      let function = call["function"]
      let index: Int
      if let known = tools[key] {
        index = known
      } else {
        events += closeOpen()
        index = nextIndex
        nextIndex += 1
        tools[key] = index
        toolOrder.append(index)
        let id = call["id"]?.string.flatMap { $0.isEmpty ? nil : $0 } ?? "call_\(index)"
        let block: JSONValue = [
          "type": "tool_use", "id": .string(id), "name": function?["name"] ?? "", "input": .object([:]),
        ]
        events.append(event("content_block_start", ["type": "content_block_start", "index": .number(Double(index)), "content_block": block]))
      }
      var arguments = function?["arguments"]?.string ?? ""
      // A server that sends the arguments as an object.
      if arguments.isEmpty, let object = function?["arguments"], object.object != nil {
        arguments = String(decoding: object.encoded(), as: UTF8.self)
      }
      if !arguments.isEmpty {
        events.append(event("content_block_delta", [
          "type": "content_block_delta", "index": .number(Double(index)),
          "delta": ["type": "input_json_delta", "partial_json": .string(arguments)],
        ]))
      }
      return events
    }

    private mutating func closeOpen() -> [Event] {
      guard let open else { return [] }
      self.open = nil
      return [event("content_block_stop", ["type": "content_block_stop", "index": .number(Double(open.index))])]
    }

    private mutating func end() -> [Event] {
      ended = true
      var events = closeOpen()
      for index in toolOrder {
        events.append(event("content_block_stop", ["type": "content_block_stop", "index": .number(Double(index))]))
      }
      let reason: String
      if !toolOrder.isEmpty {
        // Gemini says "stop" after tool calls too.
        reason = "tool_use"
      } else {
        switch finishReason {
        case "length": reason = "max_tokens"
        case "content_filter": reason = "refusal"
        default: reason = "end_turn"
        }
      }
      events.append(event("message_delta", ["type": "message_delta", "delta": ["stop_reason": .string(reason)]]))
      events.append(event("message_stop", ["type": "message_stop"]))
      return events
    }

    private func event(_ name: String, _ data: JSONValue) -> Event {
      (name, String(decoding: data.encoded(), as: UTF8.self))
    }
  }
}
