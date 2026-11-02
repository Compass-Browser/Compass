// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// The chat's conversation: the Messages API's messages, and what the sheet
/// shows of them. A port of the desktop's conversation.ts. Streamed replies
/// are put together from their events into the same content blocks the API
/// would have returned, so every block (thinking with its signature,
/// fallback blocks, tool use) goes back unchanged in the next request.
public struct Conversation: Sendable {
  public struct Turn: Sendable, Identifiable {
    public enum Role: String, Sendable { case user, assistant }
    public let id = UUID()
    public var role: Role
    /// User turns: what the user typed (empty for tool results).
    public var text = ""
    /// User turns: tool results going back, not shown as a message.
    public var isToolResults = false
    /// User turns: the page attached to it.
    public var pageTitle: String?
    public var pageURL: String?
    /// What goes to the API: content blocks (objects).
    public var content: [JSONValue] = []
    /// Assistant turns.
    public var streaming = false
    public var stopReason: String?
    public var error: String?
  }

  /// How many screenshots a request keeps; older ones become a note (each
  /// costs about 1,200 tokens, and only recent ones matter).
  public static let maxScreenshots = 2

  public private(set) var turns: [Turn] = []
  /// Partial JSON of streaming tool_use blocks, by index.
  private var partialJSON: [Int: String] = [:]

  public init() {}

  public mutating func clear() {
    turns = []
    partialJSON = [:]
  }

  /// The page's text as the user's message carries it.
  public static func pageBlock(url: String, title: String, text: String, truncated: Bool) -> String {
    func attribute(_ value: String) -> String {
      value.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "\"", with: "&quot;")
        .replacingOccurrences(of: "<", with: "&lt;").replacingOccurrences(of: ">", with: "&gt;")
    }
    return "<page url=\"\(attribute(url))\" title=\"\(attribute(title))\"\(truncated ? " truncated=\"true\"" : "")>\n\(text)\n</page>"
  }

  /// A message from the user, with the page's text first when attached.
  public mutating func addUser(text: String, page: (url: String, title: String, text: String, truncated: Bool)? = nil) {
    var turn = Turn(role: .user, text: text)
    if let page {
      turn.content.append(["type": "text", "text": .string(Self.pageBlock(url: page.url, title: page.title, text: page.text, truncated: page.truncated))])
      turn.pageTitle = page.title.isEmpty ? page.url : page.title
      turn.pageURL = page.url
    }
    turn.content.append(["type": "text", "text": .string(text)])
    turns.append(turn)
  }

  public mutating func addToolResults(_ results: [JSONValue]) {
    turns.append(Turn(role: .user, isToolResults: true, content: results))
  }

  /// The last reply's tool calls without a result (stopped, or the reply
  /// failed) each get one, or the API refuses the next request.
  public mutating func closeOpenTools(_ why: String) {
    guard let last = turns.last, last.role == .assistant else { return }
    let open = last.content.filter { $0["type"]?.string == "tool_use" && $0["id"]?.string != nil }
    guard !open.isEmpty else { return }
    addToolResults(open.map { block in
      ["type": "tool_result", "tool_use_id": block["id"]!, "content": [["type": "text", "text": .string(why)]], "is_error": true]
    })
  }

  /// The messages to send: without failed replies and the message whose
  /// reply failed (Retry sends it again); consecutive user turns (tool
  /// results, then a new message) as one message; thinking blocks without
  /// a signature (a reply stopped mid-thought) left out, since the API
  /// refuses them; only the latest screenshots.
  public func messages() -> [JSONValue] {
    var messages: [(role: Turn.Role, content: [JSONValue])] = []
    var i = 0
    while i < turns.count {
      let turn = turns[i]
      defer { i += 1 }
      if turn.role == .user, !turn.isToolResults, i + 1 < turns.count {
        let next = turns[i + 1]
        if next.role == .assistant, next.error != nil, next.content.isEmpty {
          i += 1
          continue
        }
      }
      if turn.role == .assistant, turn.streaming { continue }
      let content = turn.role == .assistant
        ? turn.content.filter { $0["type"]?.string != "thinking" || !($0["signature"]?.string ?? "").isEmpty }
        : turn.content
      if content.isEmpty { continue }
      if let last = messages.last, last.role == turn.role {
        messages[messages.count - 1].content += content
      } else {
        messages.append((turn.role, content))
      }
    }
    return Self.pruneScreenshots(messages).map { ["role": .string($0.role.rawValue), "content": .array($0.content)] }
  }

  private static func pruneScreenshots(_ messages: [(role: Turn.Role, content: [JSONValue])]) -> [(role: Turn.Role, content: [JSONValue])] {
    var kept = 0
    var messages = messages
    for m in messages.indices.reversed() where messages[m].role == .user {
      messages[m].content = messages[m].content.map { block in
        guard block["type"]?.string == "tool_result", let inner = block["content"]?.array,
          inner.contains(where: { $0["type"]?.string == "image" }), var object = block.object
        else { return block }
        if kept < maxScreenshots {
          kept += 1
          return block
        }
        object["content"] = .array(inner.map { $0["type"]?.string == "image" ? ["type": "text", "text": "(An earlier screenshot, removed.)"] : $0 })
        return .object(object)
      }
    }
    return messages
  }

  public var streamingTurn: Turn? { turns.last.flatMap { $0.role == .assistant && $0.streaming ? $0 : nil } }

  public mutating func startReply() {
    turns.append(Turn(role: .assistant, streaming: true))
    partialJSON = [:]
  }

  /// Applies a streamed event to the reply being streamed.
  public mutating func apply(event name: String, data: String) {
    guard let last = turns.indices.last, turns[last].role == .assistant, turns[last].streaming,
      let event = JSONValue.parse(data)
    else { return }
    let index = event["index"]?.number.map(Int.init)
    switch name {
    case "content_block_start":
      guard let index, var block = event["content_block"]?.object else { return }
      if block["type"] == nil { block["type"] = "text" }
      while turns[last].content.count <= index { turns[last].content.append(.null) }
      turns[last].content[index] = .object(block)
    case "content_block_delta":
      guard let index, index < turns[last].content.count, var block = turns[last].content[index].object,
        let delta = event["delta"]
      else { return }
      func append(_ field: String, _ key: String) {
        block[field] = .string((block[field]?.string ?? "") + (delta[key]?.string ?? ""))
      }
      switch delta["type"]?.string {
      case "text_delta": append("text", "text")
      case "thinking_delta": append("thinking", "thinking")
      case "signature_delta": append("signature", "signature")
      case "input_json_delta":
        partialJSON[index, default: ""] += delta["partial_json"]?.string ?? ""
        return
      default: return
      }
      turns[last].content[index] = .object(block)
    case "content_block_stop":
      guard let index, index < turns[last].content.count, var block = turns[last].content[index].object,
        let json = partialJSON.removeValue(forKey: index)
      else { return }
      block["input"] = JSONValue.parse(json.isEmpty ? "{}" : json) ?? .object([:])
      turns[last].content[index] = .object(block)
    case "message_delta":
      if let reason = event["delta"]?["stop_reason"]?.string { turns[last].stopReason = reason }
    default:
      break
    }
  }

  /// The reply is over: with `error` when it failed (an `error` event, a
  /// network or HTTP failure, or no `message_stop`).
  public mutating func finishReply(error: String? = nil) {
    guard let last = turns.indices.last, turns[last].role == .assistant, turns[last].streaming else { return }
    turns[last].streaming = false
    // Holes, if a block never started.
    turns[last].content.removeAll { $0 == .null }
    if let error { turns[last].error = error }
  }
}
