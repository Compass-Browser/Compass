// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import SwiftUI

/// Claude's replies, rendered from Markdown as the desktop's chat
/// (markdown.ts) and Android's (ClaudeMarkdown.kt) do: headings, paragraphs,
/// lists, quotes, code blocks, tables and rules; bold, italic,
/// strikethrough, inline code and links inline (`Markdown` in
/// CompassClaude). Links go through the environment's `openURL`, which the
/// sheet points at a new tab. Nothing is interpreted as HTML.
struct ClaudeMarkdownView: View {
  let text: String

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      ForEach(Array(Markdown.blocks(text).enumerated()), id: \.offset) { _, block in
        BlockView(block: block)
      }
    }
    .textSelection(.enabled)
  }

  /// `text`'s inline Markdown as styled text.
  static func attributed(_ text: String) -> AttributedString {
    var result = AttributedString()
    for span in Markdown.inline(text) {
      var part = AttributedString(span.text)
      var intent: InlinePresentationIntent = []
      if span.bold { intent.insert(.stronglyEmphasized) }
      if span.italic { intent.insert(.emphasized) }
      if span.strike { intent.insert(.strikethrough) }
      if span.code {
        intent.insert(.code)
        part.backgroundColor = Color(.tertiarySystemFill)
      }
      if !intent.isEmpty { part.inlinePresentationIntent = intent }
      if let url = span.url.flatMap(URL.init(string:)) { part.link = url }
      result += part
    }
    return result
  }
}

private struct BlockView: View {
  let block: Markdown.Block

  var body: some View {
    switch block {
    case .heading(let level, let text):
      Text(ClaudeMarkdownView.attributed(text))
        .font(level == 1 ? .title3 : level == 2 ? .headline : .subheadline)
        .fontWeight(.semibold)
        .padding(.top, 4)
    case .paragraph(let text):
      Text(ClaudeMarkdownView.attributed(text))
    case .item(let depth, let marker, let text):
      HStack(alignment: .firstTextBaseline, spacing: 6) {
        Text(marker).monospacedDigit().frame(minWidth: 14, alignment: .leading)
        Text(ClaudeMarkdownView.attributed(text))
      }
      .padding(.leading, CGFloat(depth) * 16)
    case .quote(let text):
      Text(ClaudeMarkdownView.attributed(text))
        .foregroundStyle(.secondary)
        .padding(.leading, 12)
        .overlay(alignment: .leading) {
          RoundedRectangle(cornerRadius: 1.5).fill(.quaternary).frame(width: 3)
        }
    case .code(_, let text):
      ScrollView(.horizontal, showsIndicators: false) {
        Text(text)
          .font(.system(.footnote, design: .monospaced))
          .fixedSize()
          .padding(10)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(Color(.secondarySystemFill), in: RoundedRectangle(cornerRadius: 8))
    case .table(let rows):
      ScrollView(.horizontal, showsIndicators: false) {
        Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 6) {
          ForEach(Array(rows.enumerated()), id: \.offset) { index, row in
            GridRow {
              ForEach(Array(row.enumerated()), id: \.offset) { _, cell in
                Text(ClaudeMarkdownView.attributed(cell))
                  .fontWeight(index == 0 ? .semibold : nil)
                  .frame(maxWidth: 260, alignment: .leading)
              }
            }
            if index == 0 { Divider() }
          }
        }
        .font(.footnote)
        .padding(10)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(Color(.secondarySystemFill), in: RoundedRectangle(cornerRadius: 8))
    case .rule:
      Divider()
    }
  }
}
