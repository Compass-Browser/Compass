// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertNull;
import static org.junit.Assert.assertTrue;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.annotation.Config;

import org.chromium.base.test.BaseRobolectricTestRunner;

import java.util.List;

/** Claude's Markdown in the chat (ClaudeMarkdown): blocks and inline spans. */
@RunWith(BaseRobolectricTestRunner.class)
@Config(manifest = Config.NONE)
public class MarkdownParserTest {
    private static List<MarkdownParser.Block> blocks(String text) {
        return MarkdownParser.INSTANCE.blocks(text);
    }

    private static List<MarkdownParser.Span> inline(String text) {
        return MarkdownParser.INSTANCE.inline(text);
    }

    @Test
    public void blocks() {
        List<MarkdownParser.Block> blocks =
                blocks(
                        "## Summary\n"
                                + "First line\nsame paragraph.\n\n"
                                + "- one\n  - nested\n1. first\n"
                                + "> quoted\n"
                                + "```js\nlet a = 1;\n```\n"
                                + "| A | B |\n|---|:-:|\n| 1 | 2 |\n"
                                + "---");
        assertEquals(new MarkdownParser.Block.Heading(2, "Summary"), blocks.get(0));
        assertEquals(new MarkdownParser.Block.Paragraph("First line same paragraph."), blocks.get(1));
        assertEquals(new MarkdownParser.Block.Item(0, "•", "one"), blocks.get(2));
        assertEquals(new MarkdownParser.Block.Item(1, "•", "nested"), blocks.get(3));
        assertEquals(new MarkdownParser.Block.Item(0, "1.", "first"), blocks.get(4));
        assertEquals(new MarkdownParser.Block.Quote("quoted"), blocks.get(5));
        assertEquals(new MarkdownParser.Block.Code("js", "let a = 1;"), blocks.get(6));
        MarkdownParser.Block.Table table = (MarkdownParser.Block.Table) blocks.get(7);
        assertEquals(List.of(List.of("A", "B"), List.of("1", "2")), table.getRows());
        assertEquals(MarkdownParser.Block.Rule.INSTANCE, blocks.get(8));
        assertEquals(9, blocks.size());
    }

    @Test
    public void codeStillStreaming() {
        // An unclosed fence (the reply is still coming) is code to the end.
        List<MarkdownParser.Block> blocks = blocks("Here:\n```\nline 1\nline 2");
        assertEquals(new MarkdownParser.Block.Code("", "line 1\nline 2"), blocks.get(1));
    }

    @Test
    public void inlineStyles() {
        List<MarkdownParser.Span> spans = inline("a **bold** and *it* `x*y*` ~~no~~");
        assertEquals("a ", spans.get(0).getText());
        assertTrue(spans.get(1).getBold());
        assertEquals("bold", spans.get(1).getText());
        assertTrue(spans.get(3).getItalic());
        assertTrue(spans.get(5).getCode());
        assertEquals("x*y*", spans.get(5).getText());
        assertTrue(spans.get(7).getStrike());
    }

    @Test
    public void snakeCaseAndLoneStars() {
        List<MarkdownParser.Span> spans = inline("use snake_case_name and 2 * 3 * 4");
        assertEquals(1, spans.size());
        assertFalse(spans.get(0).getItalic());
        assertEquals("use snake_case_name and 2 * 3 * 4", spans.get(0).getText());
    }

    @Test
    public void links() {
        List<MarkdownParser.Span> spans =
                inline("See [the **docs**](https://example.com/a) or https://b.example/x.");
        assertEquals("the ", spans.get(1).getText());
        assertEquals("https://example.com/a", spans.get(1).getUrl());
        assertTrue(spans.get(2).getBold());
        assertEquals("https://example.com/a", spans.get(2).getUrl());
        assertEquals("https://b.example/x", spans.get(4).getUrl());
        assertEquals(".", spans.get(5).getText());
        // Only web and mail links.
        assertNull(inline("[x](javascript:alert(1))").get(0).getUrl());
    }
}
