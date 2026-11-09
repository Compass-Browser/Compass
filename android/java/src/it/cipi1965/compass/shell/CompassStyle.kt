// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.annotation.DrawableRes
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.material3.BottomSheetDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.SheetValue
import androidx.compose.material3.rememberBottomSheetState
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
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
 * Compass's look on Android (docs/design/android.md, "Look"): the iOS app's layout, in Material
 * You: the dynamic color scheme, tonal surfaces and Material 3 shapes, and the space's tint.
 */
object CompassStyle {
    val rowHeight = 48.dp
    val rowRadius = 24.dp
    val favicon = 20.dp
    val indent = 18.dp
    val tileHeight = 56.dp
    val tileRadius = 16.dp

    /** Tiles, the profile chip, the ⋯ circle, pressed rows. */
    val fill: Color
        @Composable get() = MaterialTheme.colorScheme.surfaceContainerHighest

    /** The selected row or tile, as a navigation drawer's active item. */
    @Composable
    fun active(): Color = MaterialTheme.colorScheme.secondaryContainer

    /** Cards and tonal buttons on a sheet. */
    @Composable
    fun cardFill(): Color = MaterialTheme.colorScheme.surfaceContainerHigh

    /** A color's sidebar tint: desaturated toward its luma (as the desktop's and iOS's). */
    fun tint(color: Color): Color {
        fun channel(l: Float, v: Float) = (0.96f * (l + 0.72f * (v - l))).coerceIn(0f, 1f)
        val l = 0.299f * color.red + 0.587f * color.green + 0.114f * color.blue
        return Color(channel(l, color.red), channel(l, color.green), channel(l, color.blue))
    }

    /** "#rrggbb" as a color, or null (grey). */
    fun parse(tint: String): Color? =
        if (tint.isEmpty()) null else runCatching { Color(android.graphics.Color.parseColor(tint)) }.getOrNull()

    val greyAccent = Color(0.55f, 0.57f, 0.62f)

    /** A space's accent: its color, or the theme's primary for grey. */
    @Composable
    fun accent(tint: String): Color = parse(tint) ?: MaterialTheme.colorScheme.primary
}

/** Material You: the wallpaper's dynamic colors (Android 12+), else Material's baseline. */
@Composable
internal fun CompassTheme(dark: Boolean, content: @Composable () -> Unit) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val colors =
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.S) {
            if (dark) androidx.compose.material3.dynamicDarkColorScheme(context)
            else androidx.compose.material3.dynamicLightColorScheme(context)
        } else {
            if (dark) darkColorScheme() else lightColorScheme()
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

/** A bar floating over the page: a tonal surface (surfaceContainer) with a soft shadow. */
@Composable
fun Modifier.floatingGlass(shape: Shape): Modifier =
    this.shadow(3.dp, shape).clip(shape).background(MaterialTheme.colorScheme.surfaceContainer)

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

/** A site in a list item: its icon on a tonal circle, Material's leading avatar. */
@Composable
fun SiteTile(url: String) {
    Box(
        Modifier.size(36.dp).clip(CircleShape).background(MaterialTheme.colorScheme.surfaceContainerHighest),
        contentAlignment = Alignment.Center,
    ) {
        SiteIcon(url, 18.dp)
    }
}

/** A list item's leading icon, in Material's style: the glyph alone, in onSurfaceVariant. */
@Composable
fun MenuIcon(@DrawableRes id: Int, @Suppress("UNUSED_PARAMETER") color: Color = Color.Unspecified) {
    Box(Modifier.size(40.dp), contentAlignment = Alignment.Center) {
        Sym(id, 20f, MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/** A rounded group of rows (Theme.swift, Card). */
@Composable
fun Card(modifier: Modifier = Modifier, content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier.fillMaxWidth().clip(RoundedCornerShape(24.dp)).background(CompassStyle.cardFill()),
        content = content,
    )
}

/** A card under its section title, in Material's style (titleSmall, primary). */
@Composable
fun CardSection(title: String, content: @Composable ColumnScope.() -> Unit) {
    Column(Modifier.fillMaxWidth().padding(top = 16.dp), verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(
            title,
            style = MaterialTheme.typography.titleSmall,
            color = MaterialTheme.colorScheme.primary,
            maxLines = 1,
            modifier = Modifier.padding(start = 16.dp),
        )
        Card(content = content)
    }
}

/** A card's row, as a Material list item: leading icon, headline, supporting text, trailing. */
@Composable
fun CardRow(
    title: String,
    subtitle: String = "",
    @Suppress("UNUSED_PARAMETER") first: Boolean = false,
    @DrawableRes trailing: Int? = null,
    onClick: () -> Unit,
    icon: @Composable () -> Unit,
) {
    Row(
        Modifier.fillMaxWidth()
            .heightIn(min = if (subtitle.isEmpty()) 56.dp else 72.dp)
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        icon()
        Column(Modifier.weight(1f)) {
            Text(title, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (subtitle.isNotEmpty()) {
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
        }
        if (trailing != null) Sym(trailing, 16f, MaterialTheme.colorScheme.onSurfaceVariant)
    }
}

/**
 * A Material bottom sheet (ModalBottomSheet): scrim, drag handle, swipe down and back to close,
 * predictive back. It slides away when |open| turns false too, not only on the user's dismiss.
 * |edgeToEdge|: the content goes under the navigation bar (and pads itself), for a tinted sheet.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun CompassSheet(
    open: Boolean,
    onDismiss: () -> Unit,
    containerColor: Color = MaterialTheme.colorScheme.surfaceContainerLow,
    dragHandle: Boolean = true,
    edgeToEdge: Boolean = false,
    content: @Composable ColumnScope.() -> Unit,
) {
    val sheetState = rememberBottomSheetState(SheetValue.Hidden, setOf(SheetValue.Hidden, SheetValue.Expanded))
    var shown by remember { mutableStateOf(false) }
    LaunchedEffect(open) {
        if (open) {
            // Opened again while it was sliding away.
            if (shown) sheetState.show() else shown = true
        } else if (shown) {
            sheetState.hide()
            shown = false
        }
    }
    if (!shown) return
    val handle: (@Composable () -> Unit)? = if (dragHandle) { { BottomSheetDefaults.DragHandle() } } else null
    val insets: @Composable () -> WindowInsets =
        if (edgeToEdge) { { WindowInsets(0, 0, 0, 0) } } else { { BottomSheetDefaults.modalWindowInsets } }
    ModalBottomSheet(
        onDismissRequest = onDismiss,
        sheetState = sheetState,
        containerColor = containerColor,
        dragHandle = handle,
        contentWindowInsets = insets,
        content = content,
    )
}
