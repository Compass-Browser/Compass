// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

/**
 * The Markdown Claude writes, parsed for ClaudeMarkdown: blocks (headings, paragraphs, list items,
 * quotes, fenced code, tables, rules) and, within them, inline spans (bold, italic, strikethrough,
 * code, links). A subset, as the desktop's markdown.ts: what Claude uses, nothing as HTML.
 */
object MarkdownParser {
    sealed class Block {
        data class Heading(val level: Int, val text: String) : Block()

        data class Paragraph(val text: String) : Block()

        /** A list item: its nesting |depth| (0: top), and "•" or its number ("1."). */
        data class Item(val depth: Int, val marker: String, val text: String) : Block()

        data class Quote(val text: String) : Block()

        data class Code(val language: String, val text: String) : Block()

        /** The header row first; the separator row is left out. */
        data class Table(val rows: List<List<String>>) : Block()

        object Rule : Block()
    }

    /** A run of text with the same style; |url| for a link. */
    data class Span(
        val text: String,
        val bold: Boolean = false,
        val italic: Boolean = false,
        val strike: Boolean = false,
        val code: Boolean = false,
        val url: String? = null,
    )

    private val HEADING = Regex("^(#{1,6})\\s+(.*?)\\s*#*\\s*$")
    private val RULE = Regex("^\\s*([-*_])(\\s*\\1){2,}\\s*$")
    private val ITEM = Regex("^(\\s*)([-*+]|\\d{1,9}[.)])\\s+(.*)$")
    private val QUOTE = Regex("^\\s*>\\s?(.*)$")
    private val FENCE = Regex("^\\s*(```+|~~~+)\\s*([\\w+#.-]*).*$")
    private val TABLE_ROW = Regex("^\\s*\\|.*\\|\\s*$")
    private val TABLE_SEPARATOR = Regex("^\\s*\\|?\\s*:?-+:?\\s*(\\|\\s*:?-+:?\\s*)*\\|?\\s*$")

    fun blocks(markdown: String): List<Block> {
        val lines = markdown.replace("\r\n", "\n").split("\n")
        val blocks = mutableListOf<Block>()
        val paragraph = StringBuilder()
        fun endParagraph() {
            if (paragraph.isNotBlank()) blocks.add(Block.Paragraph(paragraph.toString().trim()))
            paragraph.clear()
        }
        var i = 0
        while (i < lines.size) {
            val line = lines[i]
            val fence = FENCE.matchEntire(line)
            if (fence != null) {
                endParagraph()
                val marker = fence.groupValues[1]
                val code = mutableListOf<String>()
                i++
                // Until the closing fence, or the end (a reply still streaming).
                while (i < lines.size && !lines[i].trim().startsWith(marker)) code.add(lines[i++])
                blocks.add(Block.Code(fence.groupValues[2], code.joinToString("\n").trimEnd()))
                i++
                continue
            }
            if (line.isBlank()) {
                endParagraph()
                i++
                continue
            }
            val heading = HEADING.matchEntire(line)
            if (heading != null) {
                endParagraph()
                blocks.add(Block.Heading(heading.groupValues[1].length, heading.groupValues[2]))
                i++
                continue
            }
            if (RULE.matches(line)) {
                endParagraph()
                blocks.add(Block.Rule)
                i++
                continue
            }
            if (TABLE_ROW.matches(line) && i + 1 < lines.size && TABLE_SEPARATOR.matches(lines[i + 1])) {
                endParagraph()
                val rows = mutableListOf(cells(line))
                i += 2
                while (i < lines.size && TABLE_ROW.matches(lines[i])) rows.add(cells(lines[i++]))
                blocks.add(Block.Table(rows))
                continue
            }
            val item = ITEM.matchEntire(line)
            if (item != null) {
                endParagraph()
                val indent = item.groupValues[1].replace("\t", "    ").length
                val marker = item.groupValues[2]
                val text = StringBuilder(item.groupValues[3])
                i++
                // Lines that continue the item: indented, not another block.
                while (i < lines.size && lines[i].isNotBlank() && lines[i].startsWith("  ") &&
                    ITEM.matchEntire(lines[i]) == null && FENCE.matchEntire(lines[i]) == null) {
                    text.append(' ').append(lines[i++].trim())
                }
                blocks.add(
                    Block.Item(
                        indent / 2,
                        if (marker[0].isDigit()) marker.dropLast(1) + "." else "•",
                        text.toString(),
                    )
                )
                continue
            }
            val quote = QUOTE.matchEntire(line)
            if (quote != null) {
                endParagraph()
                val text = StringBuilder(quote.groupValues[1])
                i++
                while (i < lines.size) {
                    val next = QUOTE.matchEntire(lines[i]) ?: break
                    text.append(if (next.groupValues[1].isBlank()) "\n" else " " + next.groupValues[1])
                    i++
                }
                blocks.add(Block.Quote(text.toString().trim()))
                continue
            }
            // A line of a paragraph; a line break where it ended with two spaces or a backslash.
            when {
                line.endsWith("  ") -> paragraph.append(line.trimEnd()).append('\n')
                line.endsWith("\\") -> paragraph.append(line.dropLast(1)).append('\n')
                else -> {
                    if (paragraph.isNotEmpty() && !paragraph.endsWith("\n")) paragraph.append(' ')
                    paragraph.append(line.trim())
                }
            }
            i++
        }
        endParagraph()
        return blocks
    }

    private fun cells(row: String): List<String> =
        row.trim().removePrefix("|").removeSuffix("|").split(Regex("(?<!\\\\)\\|")).map {
            it.trim().replace("\\|", "|")
        }

    /** |text|'s inline spans. */
    fun inline(text: String): List<Span> {
        val spans = mutableListOf<Span>()
        parseInline(text, Span(""), spans)
        // Neighbours with the same style become one.
        val merged = mutableListOf<Span>()
        for (span in spans) {
            if (span.text.isEmpty()) continue
            val last = merged.lastOrNull()
            if (last != null && last.copy(text = "") == span.copy(text = "")) {
                merged[merged.size - 1] = last.copy(text = last.text + span.text)
            } else {
                merged.add(span)
            }
        }
        return merged
    }

    private val URL = Regex("^https?://[^\\s<>()]+[^\\s<>().,;:!?'\"*_~]")

    private fun parseInline(text: String, style: Span, out: MutableList<Span>) {
        val plain = StringBuilder()
        fun flush() {
            if (plain.isNotEmpty()) out.add(style.copy(text = plain.toString()))
            plain.clear()
        }
        var i = 0
        while (i < text.length) {
            val c = text[i]
            // A backslash escapes punctuation.
            if (c == '\\' && i + 1 < text.length && !text[i + 1].isLetterOrDigit()) {
                plain.append(text[i + 1])
                i += 2
                continue
            }
            if (c == '`' && !style.code) {
                val ticks = text.substring(i).takeWhile { it == '`' }
                val end = text.indexOf(ticks, i + ticks.length)
                if (end > 0) {
                    flush()
                    out.add(style.copy(text = text.substring(i + ticks.length, end).trim(), code = true))
                    i = end + ticks.length
                    continue
                }
            }
            if (c == '[' && style.url == null) {
                val link = link(text, i)
                if (link != null) {
                    flush()
                    parseInline(link.label, style.copy(url = link.url), out)
                    i = link.end
                    continue
                }
            }
            if (c == '<' && style.url == null) {
                val end = text.indexOf('>', i)
                val inside = if (end > 0) text.substring(i + 1, end) else ""
                if (inside.startsWith("http://") || inside.startsWith("https://")) {
                    flush()
                    out.add(style.copy(text = inside, url = inside))
                    i = end + 1
                    continue
                }
            }
            if ((c == 'h') && style.url == null && (i == 0 || !text[i - 1].isLetterOrDigit())) {
                val url = URL.find(text.substring(i))?.value
                if (url != null) {
                    flush()
                    out.add(style.copy(text = url, url = url))
                    i += url.length
                    continue
                }
            }
            val delimiter =
                when {
                    text.startsWith("**", i) -> "**"
                    text.startsWith("__", i) -> "__"
                    text.startsWith("~~", i) -> "~~"
                    c == '*' -> "*"
                    c == '_' -> "_"
                    else -> null
                }
            if (delimiter != null) {
                val end = closing(text, i, delimiter)
                if (end > 0) {
                    flush()
                    val inner = text.substring(i + delimiter.length, end)
                    val next =
                        when (delimiter) {
                            "**", "__" -> style.copy(bold = true)
                            "~~" -> style.copy(strike = true)
                            else -> style.copy(italic = true)
                        }
                    parseInline(inner, next, out)
                    i = end + delimiter.length
                    continue
                }
            }
            plain.append(c)
            i++
        }
        flush()
    }

    /**
     * Where |delimiter| at |start| closes, or -1. It opens before a non-space and closes after one;
     * "_" only at word edges, so snake_case stays as it is.
     */
    private fun closing(text: String, start: Int, delimiter: String): Int {
        val from = start + delimiter.length
        if (from >= text.length || text[from].isWhitespace()) return -1
        if (delimiter[0] == '_' && start > 0 && text[start - 1].isLetterOrDigit()) return -1
        var j = from + 1
        while (j <= text.length - delimiter.length) {
            if (text.startsWith(delimiter, j) && !text[j - 1].isWhitespace()) {
                val after = j + delimiter.length
                // "*" isn't the start of a "**".
                val doubled = delimiter.length == 1 && after < text.length && text[after] == delimiter[0]
                val wordAfter = delimiter[0] == '_' && after < text.length && text[after].isLetterOrDigit()
                if (!doubled && !wordAfter) return j
                if (doubled) {
                    j += 2
                    continue
                }
            }
            if (text[j] == '`') {
                // Code spans aren't searched for delimiters.
                val close = text.indexOf('`', j + 1)
                if (close > 0) j = close
            }
            j++
        }
        return -1
    }

    private class Link(val label: String, val url: String, val end: Int)

    /** A link "[label](url)" at |start|, if there's one; only http(s) and mailto URLs. */
    private fun link(text: String, start: Int): Link? {
        var depth = 0
        var close = -1
        for (j in start until text.length) {
            when (text[j]) {
                '[' -> depth++
                ']' -> if (--depth == 0) {
                    close = j
                    break
                }
            }
        }
        if (close < 0 || close + 1 >= text.length || text[close + 1] != '(') return null
        val end = text.indexOf(')', close + 2)
        if (end < 0) return null
        val url = text.substring(close + 2, end).trim().substringBefore(' ').removeSurrounding("<", ">")
        if (!(url.startsWith("http://") || url.startsWith("https://") || url.startsWith("mailto:"))) return null
        return Link(text.substring(start + 1, close), url, end + 1)
    }
}
