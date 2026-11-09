// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.annotation.DrawableRes
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import androidx.compose.foundation.interaction.collectIsPressedAsState
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Compass's look on Android, after the iOS app's (ios/App/Views/Theme.swift; docs/design/android.md,
 * "Look"): iOS's neutral colors instead of Material You's, the space's tint, frosted floating bars,
 * cards and rows. Sizes are the iOS points as dp.
 */
object CompassStyle {
    val rowHeight = 44.dp
    val rowRadius = 10.dp
    val favicon = 18.dp
    val indent = 18.dp
    val tileHeight = 52.dp
    val tileRadius = 12.dp

    /** Tiles, pressed rows, the profile chip, the ⋯ circle. */
    val fill = Color(0xFF8E8E93).copy(alpha = 0.14f)
    val greyAccent = Color(0.55f, 0.57f, 0.62f)
    val blue = Color(0xFF007AFF)
    val orange = Color(0xFFFF9500)
    val green = Color(0xFF34C759)
    val red = Color(0xFFFF3B30)
    val purple = Color(0xFFAF52DE)
    val teal = Color(0xFF30B0C7)
    val indigo = Color(0xFF5856D6)
    val gray = Color(0xFF8E8E93)

    /** The selected row or tile: iOS's card color. */
    @Composable
    fun active(): Color = MaterialTheme.colorScheme.surfaceContainerHigh

    /** Cards on a tinted sheet. */
    @Composable
    fun cardFill(): Color = MaterialTheme.colorScheme.surfaceContainerHigh.copy(alpha = 0.82f)

    /** A color's sidebar tint: desaturated toward its luma (Theme.swift, RGB.tint). */
    fun tint(color: Color): Color {
        fun channel(l: Float, v: Float) = (0.96f * (l + 0.72f * (v - l))).coerceIn(0f, 1f)
        val l = 0.299f * color.red + 0.587f * color.green + 0.114f * color.blue
        return Color(channel(l, color.red), channel(l, color.green), channel(l, color.blue))
    }

    /** "#rrggbb" as a color, or null (grey). */
    fun parse(tint: String): Color? =
        if (tint.isEmpty()) null else runCatching { Color(android.graphics.Color.parseColor(tint)) }.getOrNull()

    /** A space's accent: its color, or grey. */
    fun accent(tint: String): Color = parse(tint) ?: greyAccent
}

/** iOS's system colors as Material's color scheme, so every Compose screen is neutral. */
@Composable
internal fun CompassTheme(dark: Boolean, content: @Composable () -> Unit) {
    val colors =
        if (dark) {
            darkColorScheme(
                primary = Color(0xFF0A84FF),
                onPrimary = Color.White,
                primaryContainer = Color(0xFF0A2A4D),
                onPrimaryContainer = Color.White,
                secondaryContainer = Color(0xFF2C2C2E),
                onSecondaryContainer = Color.White,
                background = Color.Black,
                onBackground = Color.White,
                surface = Color.Black,
                onSurface = Color.White,
                surfaceVariant = Color(0xFF2C2C2E),
                onSurfaceVariant = Color(0xFF8D8D93),
                surfaceContainerLowest = Color.Black,
                surfaceContainerLow = Color(0xFF111112),
                surfaceContainer = Color(0xFF1C1C1E),
                surfaceContainerHigh = Color(0xFF1C1C1E),
                surfaceContainerHighest = Color(0xFF2C2C2E),
                outline = Color(0xFF38383A),
                outlineVariant = Color(0xFF2C2C2E),
                error = Color(0xFFFF453A),
            )
        } else {
            lightColorScheme(
                primary = Color(0xFF007AFF),
                onPrimary = Color.White,
                primaryContainer = Color(0xFFE5F0FF),
                onPrimaryContainer = Color.Black,
                secondaryContainer = Color(0xFFE5E5EA),
                onSecondaryContainer = Color.Black,
                background = Color.White,
                onBackground = Color.Black,
                surface = Color.White,
                onSurface = Color.Black,
                surfaceVariant = Color(0xFFE5E5EA),
                onSurfaceVariant = Color(0xFF8A8A8E),
                surfaceContainerLowest = Color.White,
                surfaceContainerLow = Color(0xFFF2F2F7),
                surfaceContainer = Color(0xFFF2F2F7),
                surfaceContainerHigh = Color.White,
                surfaceContainerHighest = Color(0xFFE5E5EA),
                outline = Color(0xFFC6C6C8),
                outlineVariant = Color(0xFFE5E5EA),
                error = Color(0xFFFF3B30),
            )
        }
    MaterialTheme(colorScheme = colors, content = content)
}

/**
 * The space's tint behind a sheet (Theme.swift, SpaceTint): the system background under the
 * space color's tint, from the top left to the bottom right, fading to half; grey has none.
 */
fun Modifier.spaceTint(tint: String, background: Color, intensity: Float = 0.31f): Modifier {
    val color = CompassStyle.parse(tint) ?: return this.background(background)
    val tinted = CompassStyle.tint(color)
    val alpha = (intensity * 1.1f).coerceAtMost(1f)
    return this.background(background)
        .background(
            Brush.linearGradient(
                listOf(tinted.copy(alpha = alpha), tinted.copy(alpha = alpha * 0.5f))
            )
        )
}

/**
 * A frosted bar floating over the page (Theme.swift, floatingGlass): Compose can't blur what's
 * behind a view, so it's a nearly opaque fill with the hairline stroke and the soft shadow.
 */
@Composable
fun Modifier.floatingGlass(shape: Shape): Modifier {
    val surface = MaterialTheme.colorScheme.surfaceContainerHigh
    val onSurface = MaterialTheme.colorScheme.onSurface
    return this.shadow(10.dp, shape, ambientColor = Color.Black.copy(alpha = 0.12f), spotColor = Color.Black.copy(alpha = 0.12f))
        .clip(shape)
        .background(surface.copy(alpha = 0.94f))
        .border(0.5.dp, onSurface.copy(alpha = 0.08f), shape)
}

/** A Material Symbol (res_symbols) as SF Symbols are drawn: |pt| is the iOS point size. */
@Composable
fun Sym(@DrawableRes id: Int, pt: Float, tint: Color = MaterialTheme.colorScheme.onSurface, modifier: Modifier = Modifier) {
    // A symbol fills about 5/6 of its box; an SF Symbol's glyph is about its point size.
    Icon(painterResource(id), contentDescription = null, tint = tint, modifier = modifier.size((pt * 1.2f).dp))
}

/**
 * A site's icon (Theme.swift, SiteIcon): its favicon (Favicons), or the host's first letter on a
 * light fill.
 */
@Composable
fun SiteIcon(url: String, size: Dp = 18.dp, modifier: Modifier = Modifier) {
    val icon = Favicons.icon(url)
    val shape = RoundedCornerShape(size * 0.25f)
    if (icon != null) {
        Image(icon.asImageBitmap(), contentDescription = null, modifier = modifier.size(size).clip(shape))
        return
    }
    val letter = Favicons.hostOf(url).removePrefix("www.").take(1).uppercase().ifEmpty { "•" }
    Box(
        modifier.size(size).clip(shape).background(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.10f)),
        contentAlignment = Alignment.Center,
    ) {
        Text(
            letter,
            fontSize = (size.value * 0.6f).sp,
            fontWeight = FontWeight.SemiBold,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.7f),
            maxLines = 1,
        )
    }
}

/** A site in a row: its icon in a 32 box on a faint fill (Theme.swift, SiteTile). */
@Composable
fun SiteTile(url: String) {
    Box(
        Modifier.size(32.dp).clip(RoundedCornerShape(9.dp)).background(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.06f)),
        contentAlignment = Alignment.Center,
    ) {
        SiteIcon(url, 18.dp)
    }
}

/** A white glyph on a colored square, as iOS's Settings rows (PageMenu.swift, menuIcon). */
@Composable
fun MenuIcon(@DrawableRes id: Int, color: Color) {
    Box(
        Modifier.size(32.dp).clip(RoundedCornerShape(9.dp)).background(color),
        contentAlignment = Alignment.Center,
    ) {
        Sym(id, 15f, Color.White)
    }
}

/** A rounded group of rows (Theme.swift, Card). */
@Composable
fun Card(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier.fillMaxWidth().clip(RoundedCornerShape(16.dp)).background(CompassStyle.cardFill()),
        content = content,
    )
}

/** A card under a small uppercase title (Theme.swift, CardSection). */
@Composable
fun CardSection(title: String, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth().padding(top = 10.dp), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Text(
            title.uppercase(),
            fontSize = 12.sp,
            fontWeight = FontWeight.SemiBold,
            letterSpacing = 0.4.sp,
            color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.55f),
            maxLines = 1,
            modifier = Modifier.padding(start = 6.dp),
        )
        Card(content = content)
    }
}

/**
 * A card's row (Theme.swift, Row): a 32 icon, a title over a subtitle, a trailing glyph; a
 * hairline above all but the first; the press fills it lightly.
 */
@Composable
fun CardRow(
    title: String,
    subtitle: String = "",
    first: Boolean = false,
    @DrawableRes trailing: Int? = null,
    onClick: () -> Unit,
    icon: @Composable () -> Unit,
) {
    val interaction = remember { MutableInteractionSource() }
    val pressed by interaction.collectIsPressedAsState()
    val onSurface = MaterialTheme.colorScheme.onSurface
    Column {
        if (!first) {
            Box(Modifier.padding(start = 56.dp).fillMaxWidth().height(0.5.dp).background(MaterialTheme.colorScheme.outline.copy(alpha = 0.6f)))
        }
        Row(
            Modifier.fillMaxWidth()
                .heightIn(min = 56.dp)
                .background(if (pressed) onSurface.copy(alpha = 0.08f) else Color.Transparent)
                .clickable(interactionSource = interaction, indication = null, onClick = onClick)
                .padding(horizontal = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            icon()
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(1.dp)) {
                Text(title, fontSize = 16.sp, fontWeight = FontWeight.Medium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                if (subtitle.isNotEmpty()) {
                    Text(
                        subtitle,
                        fontSize = 13.sp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
            }
            Spacer(Modifier.width(4.dp))
            if (trailing != null) Sym(trailing, 13f, MaterialTheme.colorScheme.onSurface.copy(alpha = 0.3f))
        }
    }
}

/** The sheet's grabber: a short capsule at the top (SidebarPanel.swift). */
@Composable
fun Grabber(modifier: Modifier = Modifier) {
    Box(modifier.fillMaxWidth().height(22.dp), contentAlignment = Alignment.TopCenter) {
        Box(
            Modifier.padding(top = 7.dp)
                .size(width = 40.dp, height = 5.dp)
                .clip(RoundedCornerShape(2.5.dp))
                .background(MaterialTheme.colorScheme.onSurface.copy(alpha = 0.22f))
        )
    }
}
