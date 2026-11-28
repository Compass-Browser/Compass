// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// An HTML segment from the page translator (`page_translator.js`: a
/// block's HTML, its elements numbered with `data-compass-t`, nothing else)
/// for an engine that translates plain text only, as Apple's: its text runs
/// go one string each, and the markup stays where it was (docs/design/
/// translation.md, "iOS"). Words can't move across a link with this, which
/// Bergamot's HTML mode can.
public struct HTMLSegment: Equatable, Sendable {
  public enum Piece: Equatable, Sendable {
    case markup(String)
    /// Unescaped text.
    case text(String)
  }

  public var pieces: [Piece]

  public init(_ html: String) {
    var pieces: [Piece] = []
    var rest = Substring(html)
    while !rest.isEmpty {
      if rest.first == "<", let end = rest.firstIndex(of: ">") {
        pieces.append(.markup(String(rest[...end])))
        rest = rest[rest.index(after: end)...]
      } else {
        let end = rest.dropFirst().firstIndex(of: "<") ?? rest.endIndex
        pieces.append(.text(Self.unescape(rest[..<end])))
        rest = rest[end...]
      }
    }
    self.pieces = pieces
  }

  /// The text runs worth translating (some letters), in order.
  public var texts: [String] {
    pieces.compactMap { if case .text(let text) = $0, Self.worthTranslating(text) { text } else { nil } }
  }

  /// The segment with `translations` in place of `texts`, escaped; nil when
  /// the count differs.
  public func html(with translations: [String]) -> String? {
    guard translations.count == texts.count else { return nil }
    var next = translations.makeIterator()
    var out = ""
    for piece in pieces {
      switch piece {
      case .markup(let markup): out += markup
      case .text(let text):
        guard Self.worthTranslating(text) else {
          out += Self.escape(text)
          continue
        }
        // Keep the run's own spaces at its ends: the engine trims them,
        // and they separate words from the markup around.
        let lead = text.prefix { $0.isWhitespace }
        let trail = text.reversed().prefix { $0.isWhitespace }
        let translated = (next.next() ?? text).trimmingCharacters(in: .whitespacesAndNewlines)
        out += Self.escape(String(lead) + translated + String(trail.reversed()))
      }
    }
    return out
  }

  static func worthTranslating(_ text: String) -> Bool { text.contains { $0.isLetter } }

  /// innerHTML's escapes for text (`&amp;`, `&lt;`, `&gt;`, `&nbsp;`), and
  /// numeric references.
  static func unescape(_ text: Substring) -> String {
    guard text.contains("&") else { return String(text) }
    var out = ""
    var rest = text
    while let amp = rest.firstIndex(of: "&") {
      out += rest[..<amp]
      let after = rest[amp...]
      guard let semi = after.prefix(10).firstIndex(of: ";") else {
        out += "&"
        rest = rest[rest.index(after: amp)...]
        continue
      }
      let name = after[after.index(after: amp)..<semi]
      let named = ["amp": "&", "lt": "<", "gt": ">", "nbsp": "\u{00A0}", "quot": "\"", "apos": "'", "#39": "'"]
      if let value = named[String(name)] {
        out += value
      } else if name.hasPrefix("#"), let scalar = Self.scalar(name.dropFirst()) {
        out.unicodeScalars.append(scalar)
      } else {
        out += after[...semi]
      }
      rest = rest[rest.index(after: semi)...]
    }
    return out + rest
  }

  private static func scalar(_ number: Substring) -> Unicode.Scalar? {
    let value = number.first == "x" || number.first == "X"
      ? UInt32(number.dropFirst(), radix: 16) : UInt32(number)
    return value.flatMap(Unicode.Scalar.init)
  }

  static func escape(_ text: String) -> String {
    var out = ""
    for character in text {
      switch character {
      case "&": out += "&amp;"
      case "<": out += "&lt;"
      case ">": out += "&gt;"
      case "\u{00A0}": out += "&nbsp;"
      default: out.append(character)
      }
    }
    return out
  }
}

/// A batch of segments as plain strings for a text-only engine, and back.
public struct PlainTextBatch: Sendable {
  public struct Segment: Equatable, Sendable {
    public var text: String
    public var html: Bool
    public init(text: String, html: Bool) {
      self.text = text
      self.html = html
    }
  }

  /// Every string to translate, in order.
  public let strings: [String]
  private let segments: [Segment]
  private let parsed: [HTMLSegment?]

  public init(_ segments: [Segment]) {
    self.segments = segments
    parsed = segments.map { $0.html ? HTMLSegment($0.text) : nil }
    var strings: [String] = []
    for (segment, html) in zip(segments, parsed) {
      if let html { strings += html.texts } else { strings.append(segment.text) }
    }
    self.strings = strings
  }

  /// The segments' translations from the strings' (nil where a string had
  /// none: that segment stays as it is).
  public func translations(from results: [String?]) -> [String?] {
    guard results.count == strings.count else { return Array(repeating: nil, count: segments.count) }
    var index = 0
    return zip(segments, parsed).map { segment, html in
      if let html {
        let count = html.texts.count
        let mine = results[index..<index + count]
        index += count
        if mine.contains(where: { $0 == nil }) { return nil }
        return html.html(with: mine.map { $0! })
      }
      defer { index += 1 }
      return results[index]
    }
  }
}
