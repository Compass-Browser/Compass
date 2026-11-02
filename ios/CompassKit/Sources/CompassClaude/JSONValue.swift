// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Any JSON value. Content blocks are kept as the API sent them, every
/// field included (a thinking block's signature, fields this code doesn't
/// know), because they go back unchanged in the next request.
public enum JSONValue: Codable, Equatable, Sendable {
  case null
  case bool(Bool)
  case number(Double)
  case string(String)
  case array([JSONValue])
  case object([String: JSONValue])

  public init(from decoder: Decoder) throws {
    let container = try decoder.singleValueContainer()
    if container.decodeNil() {
      self = .null
    } else if let value = try? container.decode(Bool.self) {
      self = .bool(value)
    } else if let value = try? container.decode(Double.self) {
      self = .number(value)
    } else if let value = try? container.decode(String.self) {
      self = .string(value)
    } else if let value = try? container.decode([JSONValue].self) {
      self = .array(value)
    } else {
      self = .object(try container.decode([String: JSONValue].self))
    }
  }

  public func encode(to encoder: Encoder) throws {
    var container = encoder.singleValueContainer()
    switch self {
    case .null: try container.encodeNil()
    case .bool(let value): try container.encode(value)
    case .number(let value):
      // Whole numbers as integers (max_tokens, indexes).
      if value.rounded() == value, abs(value) < 1e15 { try container.encode(Int64(value)) } else { try container.encode(value) }
    case .string(let value): try container.encode(value)
    case .array(let value): try container.encode(value)
    case .object(let value): try container.encode(value)
    }
  }

  public var string: String? { if case .string(let value) = self { value } else { nil } }
  public var object: [String: JSONValue]? { if case .object(let value) = self { value } else { nil } }
  public var array: [JSONValue]? { if case .array(let value) = self { value } else { nil } }
  public var number: Double? { if case .number(let value) = self { value } else { nil } }

  public subscript(key: String) -> JSONValue? { object?[key] }

  /// Parses JSON text; nil if it isn't JSON.
  public static func parse(_ text: String) -> JSONValue? {
    try? JSONDecoder().decode(JSONValue.self, from: Data(text.utf8))
  }

  public static func parse(_ data: Data) -> JSONValue? {
    try? JSONDecoder().decode(JSONValue.self, from: data)
  }

  /// Compact JSON with sorted keys, so the same request is the same bytes
  /// (prompt caching matches prefixes byte for byte).
  public func encoded() -> Data {
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
    return (try? encoder.encode(self)) ?? Data("null".utf8)
  }
}

extension JSONValue: ExpressibleByStringLiteral, ExpressibleByBooleanLiteral, ExpressibleByIntegerLiteral,
  ExpressibleByArrayLiteral, ExpressibleByDictionaryLiteral
{
  public init(stringLiteral value: String) { self = .string(value) }
  public init(booleanLiteral value: Bool) { self = .bool(value) }
  public init(integerLiteral value: Int) { self = .number(Double(value)) }
  public init(arrayLiteral elements: JSONValue...) { self = .array(elements) }
  public init(dictionaryLiteral elements: (String, JSONValue)...) {
    self = .object(Dictionary(elements, uniquingKeysWith: { $1 }))
  }
}
