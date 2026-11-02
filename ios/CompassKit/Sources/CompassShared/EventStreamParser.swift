// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// The HTML spec's event-stream parsing for `event` and `data` (LF, CR or
/// CRLF line ends; comments skipped; lines capped at `maxLine` bytes, 64 KiB
/// by default). Sync's push stream and Claude's replies both use it.
public struct EventStreamParser {
  public struct Event: Equatable, Sendable {
    public var name: String
    public var data: String

    public init(name: String, data: String) {
      self.name = name
      self.data = data
    }
  }

  private let maxLine: Int

  public init(maxLine: Int = 64 * 1024) { self.maxLine = maxLine }

  private var line: [UInt8] = []
  private var lastWasCR = false
  private var name = ""
  private var data: [String] = []

  public mutating func feed(_ byte: UInt8) -> [Event] {
    if byte == 0x0a && lastWasCR {
      lastWasCR = false
      return []
    }
    lastWasCR = byte == 0x0d
    if byte == 0x0a || byte == 0x0d {
      defer { line.removeAll(keepingCapacity: true) }
      return endOfLine()
    }
    if line.count < maxLine { line.append(byte) }
    return []
  }

  private mutating func endOfLine() -> [Event] {
    if line.isEmpty {
      defer {
        name = ""
        data = []
      }
      guard !data.isEmpty else { return [] }
      return [Event(name: name.isEmpty ? "message" : name, data: data.joined(separator: "\n"))]
    }
    let text = String(decoding: line, as: UTF8.self)
    if text.hasPrefix(":") { return [] }
    let field: Substring
    var value: Substring
    if let colon = text.firstIndex(of: ":") {
      field = text[..<colon]
      value = text[text.index(after: colon)...]
      if value.hasPrefix(" ") { value = value.dropFirst() }
    } else {
      field = Substring(text)
      value = ""
    }
    switch field {
    case "event": name = String(value)
    case "data": data.append(String(value))
    default: break
    }
    return []
  }
}
