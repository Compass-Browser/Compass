// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.compose.foundation.background
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.withLink
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.TextUnit
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.em

/**
 * Claude's replies, rendered from Markdown as the desktop's chat does (resources/sidebar/claude_chat/
 * markdown.ts): headings, paragraphs, lists, quotes, code blocks, tables and rules; bold, italic,
 * strikethrough, inline code and links inline. Links go to |onLink| (a new tab), never to the
 * system. Nothing is interpreted as HTML.
 */
@Composable
fun ClaudeMarkdown(text: String, onLink: (String) -> Unit, modifier: Modifier = Modifier) {
    val blocks = remember(text) { MarkdownParser.blocks(text) }
    // The code background, and the links' color.
    val styles = listOf(MaterialTheme.colorScheme.surfaceContainerLow, MaterialTheme.colorScheme.primary)
    Column(modifier, verticalArrangement = Arrangement.spacedBy(6.dp)) {
        for (block in blocks) Block(block, styles, onLink)
    }
}

@Composable
private fun Block(block: MarkdownParser.Block, styles: List<Color>, onLink: (String) -> Unit) {
    val typography = MaterialTheme.typography
    when (block) {
        is MarkdownParser.Block.Heading ->
            Text(
                inline(block.text, styles, onLink),
                style =
                    when (block.level) {
                        1 -> typography.titleMedium
                        2 -> typography.titleSmall
                        else -> typography.labelLarge
                    },
                fontWeight = FontWeight.SemiBold,
            )
        is MarkdownParser.Block.Paragraph -> Text(inline(block.text, styles, onLink), style = typography.bodyMedium)
        is MarkdownParser.Block.Item ->
            Row(Modifier.padding(start = (block.depth * 16).dp)) {
                Text(block.marker, style = typography.bodyMedium, modifier = Modifier.width(20.dp))
                Text(inline(block.text, styles, onLink), style = typography.bodyMedium)
            }
        is MarkdownParser.Block.Quote ->
            Row(Modifier.height(IntrinsicSize.Min)) {
                Box(Modifier.width(3.dp).fillMaxHeight().background(MaterialTheme.colorScheme.outlineVariant))
                Text(
                    inline(block.text, styles, onLink),
                    style = typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(start = 10.dp),
                )
            }
        is MarkdownParser.Block.Code ->
            Text(
                block.text,
                style = typography.labelMedium,
                fontFamily = FontFamily.Monospace,
                softWrap = false,
                modifier =
                    Modifier.fillMaxWidth()
                        .background(styles[0], RoundedCornerShape(8.dp))
                        .horizontalScroll(rememberScrollState())
                        .padding(10.dp),
            )
        is MarkdownParser.Block.Table ->
            Column(
                Modifier.fillMaxWidth()
                    .background(styles[0], RoundedCornerShape(8.dp))
                    .padding(8.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                block.rows.forEachIndexed { i, row ->
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        for (cell in row) {
                            Text(
                                inline(cell, styles, onLink),
                                style = typography.bodySmall,
                                fontWeight = if (i == 0) FontWeight.SemiBold else null,
                                modifier = Modifier.weight(1f),
                            )
                        }
                    }
                    if (i == 0) HorizontalDivider()
                }
            }
        MarkdownParser.Block.Rule -> HorizontalDivider()
    }
}

/** |text|'s inline Markdown as styled text. */
private fun inline(text: String, styles: List<Color>, onLink: (String) -> Unit): AnnotatedString =
    buildAnnotatedString {
        for (span in MarkdownParser.inline(text)) {
            val style =
                SpanStyle(
                    fontWeight = if (span.bold) FontWeight.Bold else null,
                    fontStyle = if (span.italic) FontStyle.Italic else null,
                    textDecoration = if (span.strike) TextDecoration.LineThrough else null,
                    fontFamily = if (span.code) FontFamily.Monospace else null,
                    // Monospace reads larger than the text around it.
                    fontSize = if (span.code) 0.88.em else TextUnit.Unspecified,
                    background = if (span.code) styles[0] else Color.Unspecified,
                )
            val url = span.url
            if (url != null) {
                withLink(
                    LinkAnnotation.Clickable(
                        url,
                        TextLinkStyles(SpanStyle(color = styles[1], textDecoration = TextDecoration.Underline)),
                    ) {
                        onLink(url)
                    }
                ) {
                    withStyle(style) { append(span.text) }
                }
            } else {
                withStyle(style) { append(span.text) }
            }
        }
    }
