// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Testing

@testable import CompassClaude

/// Claude's Markdown in the chat sheet: Android's MarkdownParserTest, and a
/// few more.
struct MarkdownTests {
  @Test func blocks() {
    let blocks = Markdown.blocks(
      "## Summary\n" + "First line\nsame paragraph.\n\n" + "- one\n  - nested\n1. first\n" + "> quoted\n"
        + "```js\nlet a = 1;\n```\n" + "| A | B |\n|---|:-:|\n| 1 | 2 |\n" + "---")
    #expect(
      blocks == [
        .heading(level: 2, text: "Summary"),
        .paragraph("First line same paragraph."),
        .item(depth: 0, marker: "•", text: "one"),
        .item(depth: 1, marker: "•", text: "nested"),
        .item(depth: 0, marker: "1.", text: "first"),
        .quote("quoted"),
        .code(language: "js", text: "let a = 1;"),
        .table([["A", "B"], ["1", "2"]]),
        .rule,
      ])
  }

  @Test func codeStillStreaming() {
    // An unclosed fence (the reply is still coming) is code to the end.
    #expect(Markdown.blocks("Here:\n```\nline 1\nline 2")[1] == .code(language: "", text: "line 1\nline 2"))
  }

  @Test func lineBreaksContinuationsAndEscapedPipes() {
    #expect(Markdown.blocks("one  \ntwo\\\nthree") == [.paragraph("one\ntwo\nthree")])
    #expect(Markdown.blocks("- an item\n  that goes on\n2) second") == [
      .item(depth: 0, marker: "•", text: "an item that goes on"),
      .item(depth: 0, marker: "2.", text: "second"),
    ])
    #expect(Markdown.blocks("| a \\| b | c |\n| - | - |") == [.table([["a | b", "c"]])])
    #expect(Markdown.blocks("> one\n>\n> two") == [.quote("one\n two")])
  }

  @Test func inlineStyles() {
    let spans = Markdown.inline("a **bold** and *it* `x*y*` ~~no~~")
    #expect(spans[0].text == "a ")
    #expect(spans[1] == Markdown.Span("bold", bold: true))
    #expect(spans[3] == Markdown.Span("it", italic: true))
    #expect(spans[5] == Markdown.Span("x*y*", code: true))
    #expect(spans[7] == Markdown.Span("no", strike: true))
  }

  @Test func snakeCaseAndLoneStars() {
    #expect(
      Markdown.inline("use snake_case_name and 2 * 3 * 4") == [Markdown.Span("use snake_case_name and 2 * 3 * 4")])
    #expect(Markdown.inline("\\*not\\* italic") == [Markdown.Span("*not* italic")])
  }

  @Test func links() {
    let spans = Markdown.inline("See [the **docs**](https://example.com/a) or https://b.example/x.")
    #expect(spans[1] == Markdown.Span("the ", url: "https://example.com/a"))
    #expect(spans[2] == Markdown.Span("docs", bold: true, url: "https://example.com/a"))
    #expect(spans[4] == Markdown.Span("https://b.example/x", url: "https://b.example/x"))
    #expect(spans[5].text == ".")
    // Only web and mail links.
    #expect(Markdown.inline("[x](javascript:alert(1))")[0].url == nil)
    #expect(Markdown.inline("<https://c.example>") == [Markdown.Span("https://c.example", url: "https://c.example")])
  }
}
