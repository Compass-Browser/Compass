// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// The Markdown Claude writes, parsed for the chat sheet: blocks (headings,
/// paragraphs, list items, quotes, fenced code, tables, rules) and, within
/// them, inline spans (bold, italic, strikethrough, code, links). A subset,
/// as the desktop's markdown.ts and Android's MarkdownParser.kt (this is a
/// port of the latter): what Claude uses, nothing as HTML.
public enum Markdown {
  public enum Block: Equatable, Sendable {
    case heading(level: Int, text: String)
    case paragraph(String)
    /// A list item: its nesting `depth` (0: top), and "•" or its number
    /// ("1.").
    case item(depth: Int, marker: String, text: String)
    case quote(String)
    case code(language: String, text: String)
    /// The header row first; the separator row is left out.
    case table([[String]])
    case rule
  }

  /// A run of text with the same style; `url` for a link.
  public struct Span: Equatable, Sendable {
    public var text: String
    public var bold = false
    public var italic = false
    public var strike = false
    public var code = false
    public var url: String?

    public init(
      _ text: String, bold: Bool = false, italic: Bool = false, strike: Bool = false, code: Bool = false,
      url: String? = nil
    ) {
      self.text = text
      self.bold = bold
      self.italic = italic
      self.strike = strike
      self.code = code
      self.url = url
    }

    func sameStyle(as other: Span) -> Bool {
      bold == other.bold && italic == other.italic && strike == other.strike && code == other.code
        && url == other.url
    }
  }

  // MARK: Blocks

  private static func regex(_ pattern: String) -> NSRegularExpression {
    // The patterns are constants.
    try! NSRegularExpression(pattern: pattern)
  }

  private static let heading = regex(#"^(#{1,6})\s+(.*?)\s*#*\s*$"#)
  private static let rule = regex(#"^\s*([-*_])(\s*\1){2,}\s*$"#)
  private static let item = regex(#"^(\s*)([-*+]|\d{1,9}[.)])\s+(.*)$"#)
  private static let quote = regex(#"^\s*>\s?(.*)$"#)
  private static let fence = regex(#"^\s*(```+|~~~+)\s*([\w+#.-]*).*$"#)
  private static let tableRow = regex(#"^\s*\|.*\|\s*$"#)
  private static let tableSeparator = regex(#"^\s*\|?\s*:?-+:?\s*(\|\s*:?-+:?\s*)*\|?\s*$"#)
  private static let cellSeparator = regex(#"(?<!\\)\|"#)
  private static let bareURL = regex(#"^https?://[^\s<>()]+[^\s<>().,;:!?'"*_~]"#)

  /// The groups of `regex` matching the whole of `line` (the group 0 first),
  /// or nil.
  private static func match(_ regex: NSRegularExpression, _ line: String) -> [String]? {
    let range = NSRange(line.startIndex..., in: line)
    guard let result = regex.firstMatch(in: line, range: range), result.range == range else { return nil }
    return (0..<result.numberOfRanges).map { i in
      Range(result.range(at: i), in: line).map { String(line[$0]) } ?? ""
    }
  }

  private static func isBlank(_ text: some StringProtocol) -> Bool {
    text.allSatisfy(\.isWhitespace)
  }

  private static func trimmed(_ text: some StringProtocol) -> String {
    text.trimmingCharacters(in: .whitespacesAndNewlines)
  }

  public static func blocks(_ markdown: String) -> [Block] {
    let lines = markdown.replacingOccurrences(of: "\r\n", with: "\n").components(separatedBy: "\n")
    var blocks: [Block] = []
    var paragraph = ""
    func endParagraph() {
      if !isBlank(paragraph) { blocks.append(.paragraph(trimmed(paragraph))) }
      paragraph = ""
    }
    var i = 0
    while i < lines.count {
      let line = lines[i]
      if let fence = match(fence, line) {
        endParagraph()
        let marker = fence[1]
        var code: [String] = []
        i += 1
        // Until the closing fence, or the end (a reply still streaming).
        while i < lines.count && !trimmed(lines[i]).hasPrefix(marker) {
          code.append(lines[i])
          i += 1
        }
        let text = code.joined(separator: "\n")
        blocks.append(.code(language: fence[2], text: String(text.reversed().drop(while: \.isWhitespace).reversed())))
        i += 1
        continue
      }
      if isBlank(line) {
        endParagraph()
        i += 1
        continue
      }
      if let heading = match(heading, line) {
        endParagraph()
        blocks.append(.heading(level: heading[1].count, text: heading[2]))
        i += 1
        continue
      }
      if match(rule, line) != nil {
        endParagraph()
        blocks.append(.rule)
        i += 1
        continue
      }
      if match(tableRow, line) != nil && i + 1 < lines.count && match(tableSeparator, lines[i + 1]) != nil {
        endParagraph()
        var rows = [cells(line)]
        i += 2
        while i < lines.count && match(tableRow, lines[i]) != nil {
          rows.append(cells(lines[i]))
          i += 1
        }
        blocks.append(.table(rows))
        continue
      }
      if let item = match(item, line) {
        endParagraph()
        let indent = item[1].replacingOccurrences(of: "\t", with: "    ").count
        let marker = item[2]
        var text = item[3]
        i += 1
        // Lines that continue the item: indented, not another block.
        while i < lines.count, !isBlank(lines[i]), lines[i].hasPrefix("  "), match(self.item, lines[i]) == nil,
          match(self.fence, lines[i]) == nil
        {
          text += " " + trimmed(lines[i])
          i += 1
        }
        let shown = marker.first?.isNumber == true ? String(marker.dropLast()) + "." : "•"
        blocks.append(.item(depth: indent / 2, marker: shown, text: text))
        continue
      }
      if let quote = match(quote, line) {
        endParagraph()
        var text = quote[1]
        i += 1
        while i < lines.count, let next = match(self.quote, lines[i]) {
          text += isBlank(next[1]) ? "\n" : " " + next[1]
          i += 1
        }
        blocks.append(.quote(trimmed(text)))
        continue
      }
      // A line of a paragraph; a line break where it ended with two spaces or
      // a backslash.
      if line.hasSuffix("  ") {
        paragraph += String(line.reversed().drop(while: \.isWhitespace).reversed()) + "\n"
      } else if line.hasSuffix("\\") {
        paragraph += String(line.dropLast()) + "\n"
      } else {
        if !paragraph.isEmpty && !paragraph.hasSuffix("\n") { paragraph += " " }
        paragraph += trimmed(line)
      }
      i += 1
    }
    endParagraph()
    return blocks
  }

  private static func cells(_ row: String) -> [String] {
    var text = trimmed(row)
    if text.hasPrefix("|") { text.removeFirst() }
    if text.hasSuffix("|") { text.removeLast() }
    var cells: [String] = []
    var start = text.startIndex
    for result in cellSeparator.matches(in: text, range: NSRange(text.startIndex..., in: text)) {
      guard let range = Range(result.range, in: text) else { continue }
      cells.append(String(text[start..<range.lowerBound]))
      start = range.upperBound
    }
    cells.append(String(text[start...]))
    return cells.map { trimmed($0).replacingOccurrences(of: "\\|", with: "|") }
  }

  // MARK: Inline

  /// `text`'s inline spans.
  public static func inline(_ text: String) -> [Span] {
    var spans: [Span] = []
    parseInline(Array(text), style: Span(""), into: &spans)
    // Neighbours with the same style become one.
    var merged: [Span] = []
    for span in spans where !span.text.isEmpty {
      if let last = merged.last, last.sameStyle(as: span) {
        merged[merged.count - 1].text += span.text
      } else {
        merged.append(span)
      }
    }
    return merged
  }

  private static func isWord(_ c: Character) -> Bool { c.isLetter || c.isNumber }

  private static func starts(_ chars: [Character], _ prefix: [Character], at index: Int) -> Bool {
    index >= 0 && index + prefix.count <= chars.count && Array(chars[index..<index + prefix.count]) == prefix
  }

  private static func index(of needle: [Character], in chars: [Character], from: Int) -> Int? {
    guard !needle.isEmpty, from >= 0 else { return nil }
    var j = from
    while j + needle.count <= chars.count {
      if starts(chars, needle, at: j) { return j }
      j += 1
    }
    return nil
  }

  private static func parseInline(_ text: [Character], style: Span, into out: inout [Span]) {
    var plain = ""
    func flush() {
      if !plain.isEmpty {
        var span = style
        span.text = plain
        out.append(span)
      }
      plain = ""
    }
    func with(_ text: String, code: Bool? = nil, url: String? = nil) -> Span {
      var span = style
      span.text = text
      if let code { span.code = code }
      if let url { span.url = url }
      return span
    }
    var i = 0
    while i < text.count {
      let c = text[i]
      // A backslash escapes punctuation.
      if c == "\\" && i + 1 < text.count && !isWord(text[i + 1]) {
        plain.append(text[i + 1])
        i += 2
        continue
      }
      if c == "`" && !style.code {
        var ticks: [Character] = []
        while i + ticks.count < text.count && text[i + ticks.count] == "`" { ticks.append("`") }
        if let end = index(of: ticks, in: text, from: i + ticks.count) {
          flush()
          out.append(with(trimmed(String(text[(i + ticks.count)..<end])), code: true))
          i = end + ticks.count
          continue
        }
      }
      if c == "[" && style.url == nil, let link = link(text, at: i) {
        flush()
        var next = style
        next.url = link.url
        parseInline(link.label, style: next, into: &out)
        i = link.end
        continue
      }
      if c == "<" && style.url == nil, let end = index(of: [">"], in: text, from: i) {
        let inside = String(text[(i + 1)..<end])
        if inside.hasPrefix("http://") || inside.hasPrefix("https://") {
          flush()
          out.append(with(inside, url: inside))
          i = end + 1
          continue
        }
      }
      if c == "h" && style.url == nil && (i == 0 || !isWord(text[i - 1])),
        let url = match(bareURLPrefix: String(text[i...]))
      {
        flush()
        out.append(with(url, url: url))
        i += url.count
        continue
      }
      let delimiter: [Character]? =
        starts(text, ["*", "*"], at: i)
        ? ["*", "*"]
        : starts(text, ["_", "_"], at: i)
          ? ["_", "_"]
          : starts(text, ["~", "~"], at: i) ? ["~", "~"] : (c == "*" || c == "_") ? [c] : nil
      if let delimiter, let end = closing(text, start: i, delimiter: delimiter) {
        flush()
        let inner = Array(text[(i + delimiter.count)..<end])
        var next = style
        switch delimiter {
        case ["*", "*"], ["_", "_"]: next.bold = true
        case ["~", "~"]: next.strike = true
        default: next.italic = true
        }
        parseInline(inner, style: next, into: &out)
        i = end + delimiter.count
        continue
      }
      plain.append(c)
      i += 1
    }
    flush()
  }

  private static func match(bareURLPrefix text: String) -> String? {
    guard let result = bareURL.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
      let range = Range(result.range, in: text)
    else { return nil }
    return String(text[range])
  }

  /// Where `delimiter` at `start` closes, or nil. It opens before a
  /// non-space and closes after one; "_" only at word edges, so snake_case
  /// stays as it is.
  private static func closing(_ text: [Character], start: Int, delimiter: [Character]) -> Int? {
    let from = start + delimiter.count
    if from >= text.count || text[from].isWhitespace { return nil }
    if delimiter[0] == "_" && start > 0 && isWord(text[start - 1]) { return nil }
    var j = from + 1
    while j <= text.count - delimiter.count {
      if starts(text, delimiter, at: j) && !text[j - 1].isWhitespace {
        let after = j + delimiter.count
        // "*" isn't the start of a "**".
        let doubled = delimiter.count == 1 && after < text.count && text[after] == delimiter[0]
        let wordAfter = delimiter[0] == "_" && after < text.count && isWord(text[after])
        if !doubled && !wordAfter { return j }
        if doubled {
          j += 2
          continue
        }
      }
      if text[j] == "`", let close = index(of: ["`"], in: text, from: j + 1) {
        // Code spans aren't searched for delimiters.
        j = close
      }
      j += 1
    }
    return nil
  }

  /// A link "[label](url)" at `start`, if there's one; only http(s) and
  /// mailto URLs.
  private static func link(_ text: [Character], at start: Int) -> (label: [Character], url: String, end: Int)? {
    var depth = 0
    var close: Int?
    for j in start..<text.count {
      if text[j] == "[" {
        depth += 1
      } else if text[j] == "]" {
        depth -= 1
        if depth == 0 {
          close = j
          break
        }
      }
    }
    guard let close, close + 1 < text.count, text[close + 1] == "(",
      let end = index(of: [")"], in: text, from: close + 2)
    else { return nil }
    var url = trimmed(String(text[(close + 2)..<end]))
    if let space = url.firstIndex(of: " ") { url = String(url[..<space]) }
    if url.hasPrefix("<") && url.hasSuffix(">") && url.count >= 2 { url = String(url.dropFirst().dropLast()) }
    guard url.hasPrefix("http://") || url.hasPrefix("https://") || url.hasPrefix("mailto:") else { return nil }
    return (Array(text[(start + 1)..<close]), url, end + 1)
  }
}
