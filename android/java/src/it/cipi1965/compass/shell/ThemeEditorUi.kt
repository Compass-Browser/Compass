// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import androidx.annotation.DrawableRes
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.luminance
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.selected
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.foundation.text.input.TextFieldLineLimits
import androidx.compose.foundation.text.input.rememberTextFieldState
import androidx.compose.foundation.text.input.TextFieldState
import androidx.compose.foundation.text.input.InputTransformation
import androidx.compose.foundation.text.input.maxLengthTrim
import androidx.compose.material3.SliderState
import it.cipi1965.compass.R
import kotlinx.coroutines.delay
import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.roundToInt
import kotlin.math.sin

/**
 * A space's theme, edited as on the desktop and iOS (docs/design/spaces.md, "Theme editor";
 * ThemeEditor.swift): the mode, a pad with the space's colors as dots (hue round the center,
 * saturation out from it), one to three colors, the palette and lightness, whole gradients,
 * intensity and grain, and the space's emoji. What changes shows at once, here and in the
 * sidebar behind, and goes to the space (and sync) at most every 50 ms.
 */
@Composable
internal fun ThemeEditor(
    open: Boolean,
    space: SidebarState.Space,
    onTheme: (SpaceTheme) -> Unit,
    onEmoji: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    // The editor's own copy: what's dragged shows at once, the space follows.
    var draft by remember(space.id, open) { mutableStateOf(space.theme) }
    val emojiField = remember(space.id, open) { TextFieldState(space.emoji) }
    val push by rememberUpdatedState(onTheme)
    if (open) {
        // At most every 50 ms (the desktop's CUSTOM_COLOR_MS), and the last one when it closes.
        LaunchedEffect(space.id) {
            var sent = space.theme
            snapshotFlow { draft }.collect {
                if (it != sent) {
                    sent = it
                    push(it)
                    delay(50)
                }
            }
        }
        DisposableEffect(space.id) {
            onDispose {
                if (draft != space.theme) push(draft)
            }
        }
    }
    val close = {
        if (draft != space.theme) onTheme(draft)
        val typed = emojiField.text.toString().trim()
        if (typed != space.emoji) onEmoji(typed)
        onDismiss()
    }
    CompassSheet(open, onDismiss = close, dragHandle = false, edgeToEdge = true) {
        SpaceThemed(draft) {
            Column(
                Modifier.fillMaxSize()
                    .spaceTint(draft) { it.surfaceContainerLow }
                    // Full height: the sheet goes under the system bars, its content doesn't.
                    .windowInsetsPadding(WindowInsets.statusBars.union(WindowInsets.navigationBars))
            ) {
                Row(
                    Modifier.fillMaxWidth().padding(start = 20.dp, end = 8.dp, top = 12.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        space.name.ifEmpty { "Space" },
                        style = MaterialTheme.typography.titleLarge,
                        modifier = Modifier.weight(1f),
                    )
                    TextButton(onClick = close) { Text("Done") }
                }
                Column(
                    Modifier.fillMaxWidth().verticalScroll(rememberScrollState()).padding(16.dp),
                    verticalArrangement = Arrangement.spacedBy(20.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                ) {
                    ModePicker(draft) { draft = draft.copy(appearance = it) }
                    ThemePad(draft, Modifier.widthIn(max = 300.dp).padding(horizontal = 24.dp)) { draft = it }
                    CountControls(draft) { draft = it }
                    Palette(draft) { draft = it }
                    Presets(draft) { draft = it }
                    if (!draft.isGrey) {
                        CardSection("Look") {
                            SliderRow("Intensity", R.drawable.compass_sym_contrast, draft.intensity, draft.accent) {
                                draft = draft.copy(intensity = it)
                            }
                            SliderRow("Grain", R.drawable.compass_sym_grain, draft.grain, draft.accent) {
                                draft = draft.copy(grain = it)
                            }
                        }
                    }
                    CardSection("Icon") {
                        OutlinedTextField(
                            state = emojiField,
                            // An emoji or two (8 UTF-16 units, as before).
                            inputTransformation = InputTransformation.maxLengthTrim(8),
                            label = { Text("Emoji") },
                            placeholder = { Text("The name's first letter") },
                            lineLimits = TextFieldLineLimits.SingleLine,
                            modifier = Modifier.fillMaxWidth().padding(14.dp),
                        )
                    }
                    Spacer(Modifier.heightIn(min = 24.dp))
                }
            }
        }
    }
}

private val SpaceTheme.Appearance.symbol: Int
    @DrawableRes get() = when (this) {
        SpaceTheme.Appearance.AUTO -> R.drawable.compass_sym_auto_awesome
        SpaceTheme.Appearance.LIGHT -> R.drawable.compass_sym_light_mode
        SpaceTheme.Appearance.DARK -> R.drawable.compass_sym_dark_mode
    }

@Composable
private fun ModePicker(theme: SpaceTheme, onPick: (SpaceTheme.Appearance) -> Unit) {
    Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
        for (mode in SpaceTheme.Appearance.entries) {
            val selected = theme.appearance == mode
            val shape = RoundedCornerShape(12.dp)
            Column(
                Modifier.weight(1f)
                    .heightIn(min = 54.dp)
                    .clip(shape)
                    .background(if (selected) CompassStyle.cardFill() else CompassStyle.fill)
                    .border(1.5.dp, theme.accent.copy(alpha = if (selected) 0.6f else 0f), shape)
                    .clickable { onPick(mode) }
                    .semantics {
                        contentDescription = mode.label
                        this.selected = selected
                    }
                    .padding(vertical = 8.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp, Alignment.CenterVertically),
            ) {
                // The space's color when it reads on the button (a dark color in dark mode doesn't).
                val onButton = MaterialTheme.colorScheme.onSurface
                val color = if (selected && contrast(theme.accent, CompassStyle.cardFill()) >= 3f) theme.accent else onButton
                Sym(mode.symbol, 17f, color)
                Text(mode.label, fontSize = 12.sp, fontWeight = FontWeight.Medium, color = color)
            }
        }
    }
}

/** − and +: one color less or more, 1 to 3 (the new one 40° on from the last). */
@Composable
private fun CountControls(theme: SpaceTheme, onChange: (SpaceTheme) -> Unit) {
    Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(18.dp)) {
        RoundButton(R.drawable.compass_sym_remove, "Fewer colors", theme.colors.size > 1) {
            onChange(theme.copy(colors = theme.colors.dropLast(1)))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(5.dp)) {
            for (i in 0 until SpaceTheme.MAX_COLORS) {
                Box(
                    Modifier.size(8.dp).clip(CircleShape).background(
                        theme.colors.getOrNull(i)?.color ?: MaterialTheme.colorScheme.onSurface.copy(alpha = 0.15f)
                    )
                )
            }
        }
        RoundButton(R.drawable.compass_sym_add, "More colors", theme.colors.size < SpaceTheme.MAX_COLORS) {
            val last = theme.colors.lastOrNull()
            onChange(theme.copy(colors = if (last == null) listOf(Rgb.parse(SpaceTheme.PALETTE[1])!!) else theme.colors + last.rotated(40.0)))
        }
    }
}

@Composable
private fun RoundButton(@DrawableRes symbol: Int, label: String, enabled: Boolean, onClick: () -> Unit) {
    Box(
        Modifier.size(40.dp)
            .clip(CircleShape)
            .background(CompassStyle.fill)
            .clickable(enabled = enabled, onClick = onClick)
            .semantics { contentDescription = label },
        contentAlignment = Alignment.Center,
    ) {
        Sym(symbol, 15f, MaterialTheme.colorScheme.onSurface.copy(alpha = if (enabled) 1f else 0.35f))
    }
}

/**
 * Grey and the eight colors (a palette color replaces the gradient), and the color's lightness:
 * with the pad's hue and saturation, any color (iOS's "Any Color", the desktop's color well).
 */
@Composable
private fun Palette(theme: SpaceTheme, onChange: (SpaceTheme) -> Unit) {
    CardSection("Colors") {
        Row(Modifier.fillMaxWidth().padding(horizontal = 6.dp, vertical = 4.dp)) {
            SpaceTheme.PALETTE.forEachIndexed { i, hex ->
                val selected = theme.palette == hex
                val rgb = Rgb.parse(hex)
                Box(
                    Modifier.weight(1f).heightIn(min = 44.dp)
                        .clickable { onChange(theme.copy(colors = listOfNotNull(rgb))) }
                        .semantics {
                            contentDescription = SpaceTheme.PALETTE_NAMES[i]
                            this.selected = selected
                        },
                    contentAlignment = Alignment.Center,
                ) {
                    Box(
                        Modifier.size(32.dp)
                            .border(2.dp, MaterialTheme.colorScheme.onSurface.copy(alpha = if (selected) 0.7f else 0f), CircleShape)
                            .padding(3.dp)
                            .clip(CircleShape)
                            .background(rgb?.color ?: CompassStyle.greyAccent)
                    )
                }
            }
        }
        val color = theme.color
        if (color != null) {
            SliderRow("Lightness", R.drawable.compass_sym_light_mode, color.hsl.lightness, theme.accent) { l ->
                val hsl = color.hsl
                onChange(theme.copy(colors = listOf(Rgb.fromHsl(hsl.copy(lightness = l))) + theme.gradient))
            }
        }
    }
}

@Composable
private fun Presets(theme: SpaceTheme, onChange: (SpaceTheme) -> Unit) {
    CardSection("Gradients") {
        Row(
            Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = 14.dp, vertical = 12.dp),
            horizontalArrangement = Arrangement.spacedBy(14.dp),
        ) {
            for ((name, colors) in SpaceTheme.PRESETS) {
                val selected = theme.colors == colors
                val shape = RoundedCornerShape(10.dp)
                Column(
                    Modifier.clickable { onChange(theme.copy(colors = colors)) }.semantics { contentDescription = name },
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(5.dp),
                ) {
                    Box(
                        Modifier.size(56.dp, 38.dp)
                            .clip(shape)
                            .background(Brush.linearGradient(SpaceTheme(colors).stops.map { it.color }))
                            .border(2.dp, MaterialTheme.colorScheme.onSurface.copy(alpha = if (selected) 0.7f else 0f), shape)
                    )
                    Text(name, fontSize = 11.sp, fontWeight = FontWeight.Medium, color = MaterialTheme.colorScheme.onSurfaceVariant)
                }
            }
        }
    }
}

@Composable
private fun SliderRow(title: String, @DrawableRes symbol: Int, value: Double, accent: Color, onChange: (Double) -> Unit) {
    Row(
        Modifier.fillMaxWidth().heightIn(min = 52.dp).padding(horizontal = 14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Sym(symbol, 17f, MaterialTheme.colorScheme.onSurfaceVariant)
        Text(title, fontSize = 16.sp, fontWeight = FontWeight.Medium, modifier = Modifier.width(84.dp))
        // The slider's own state follows |value|, and reports the user's moves.
        val slider = remember { SliderState(value = value.toFloat()) }
        LaunchedEffect(value) { if (slider.value != value.toFloat()) slider.value = value.toFloat() }
        Slider(
            state = slider,
            onValueChange = {
                slider.value = it
                onChange(it.toDouble())
            },
            colors = SliderDefaults.colors(thumbColor = accent, activeTrackColor = accent),
            modifier = Modifier.weight(1f).semantics { contentDescription = title },
        )
        Text(
            "${(value * 100).roundToInt()}",
            fontSize = 13.sp,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(30.dp),
        )
    }
}

/**
 * The pad (ThemePad in ThemeEditor.swift): the theme's colors as dots on a hue wheel, hue as the
 * angle round the center (red at the top, clockwise), saturation as the distance. The big dot is
 * the color, the small ones the gradient. Dragging a dot keeps its lightness; dragging the big
 * one turns and pushes the others by as much, so the gradient keeps its shape. A touch away from
 * the dots moves the big one there; on a grey space it makes the first color, at 55% lightness.
 */
@Composable
private fun ThemePad(theme: SpaceTheme, modifier: Modifier, onChange: (SpaceTheme) -> Unit) {
    val density = LocalDensity.current
    val big = with(density) { 34.dp.toPx() }
    val small = with(density) { 24.dp.toPx() }
    val hit = with(density) { 26.dp.toPx() }
    val current by rememberUpdatedState(theme)
    var dragged by remember { mutableStateOf(-1) }
    val backdrop = MaterialTheme.colorScheme.surfaceContainerLow
    val outline = MaterialTheme.colorScheme.onSurface
    BoxWithConstraints(modifier.fillMaxWidth().aspectRatio(1f).semantics { contentDescription = "Color pad" }) {
        val side = with(density) { minOf(maxWidth, maxHeight).toPx() }
        val radius = side / 2
        val reach = radius - big / 2

        fun point(c: Rgb): Offset {
            val hsl = c.hsl
            val angle = Math.toRadians(hsl.hue)
            val d = hsl.saturation * reach
            return Offset((radius + sin(angle) * d).toFloat(), (radius - cos(angle) * d).toFloat())
        }

        fun hueSaturation(p: Offset): Pair<Double, Double> {
            val dx = (p.x - radius).toDouble()
            val dy = (p.y - radius).toDouble()
            val hue = (Math.toDegrees(atan2(dx, -dy)) + 360) % 360
            return hue to minOf(hypot(dx, dy) / reach, 1.0)
        }

        // Dot |index| at |p|, keeping its lightness; the big one takes the others along.
        fun move(index: Int, start: List<Rgb>, p: Offset) {
            if (index !in start.indices) return
            val (hue, sat) = hueSaturation(p)
            val was = start[index].hsl
            val colors = start.toMutableList()
            colors[index] = Rgb.fromHsl(Rgb.Hsl(hue, sat, was.lightness))
            if (index == 0) {
                val dHue = hue - was.hue
                val dSat = sat - was.saturation
                for (i in 1 until colors.size) {
                    val h = start[i].hsl
                    colors[i] = Rgb.fromHsl(h.copy(hue = h.hue + dHue, saturation = h.saturation + dSat))
                }
            }
            onChange(current.copy(colors = colors))
        }

        Canvas(
            Modifier.fillMaxSize().pointerInput(radius) {
                awaitEachGesture {
                    val down = awaitFirstDown()
                    down.consume()
                    var start = current.colors
                    // The dot under the finger, small ones first (they're drawn on top).
                    var index = start.indices.reversed().firstOrNull { i ->
                        (point(start[i]) - down.position).getDistance() < hit
                    } ?: -1
                    if (index < 0) {
                        if (start.isEmpty()) start = listOf(Rgb.fromHsl(Rgb.Hsl(0.0, 0.0, 0.55)))
                        index = 0
                        move(0, start, down.position)
                        start = current.colors.ifEmpty { start }
                    }
                    dragged = index
                    while (true) {
                        val event = awaitPointerEvent()
                        val change = event.changes.firstOrNull { it.id == down.id } ?: break
                        if (!change.pressed) break
                        change.consume()
                        move(index, start, change.position)
                    }
                    dragged = -1
                }
            }
        ) {
            val center = Offset(radius, radius)
            // The hue wheel: red at the top, clockwise (a sweep starts at 3 o'clock, hue 90).
            val wheel = (0..12).map { Rgb.fromHsl(Rgb.Hsl(90.0 + it * 30, 0.75, 0.6)).color }
            drawCircle(Brush.sweepGradient(wheel, center), radius, center, alpha = 0.55f)
            drawCircle(Brush.radialGradient(listOf(backdrop, backdrop.copy(alpha = 0f)), center, radius), radius, center, alpha = 0.55f)
            drawCircle(outline.copy(alpha = 0.08f), radius - 0.5f, center, style = Stroke(1.dp.toPx()))
            val colors = theme.colors
            if (colors.size > 1) {
                val points = colors.map(::point)
                for (i in 0 until points.size - 1) {
                    drawLine(
                        outline.copy(alpha = 0.35f), points[i], points[i + 1], 1.5.dp.toPx(),
                        pathEffect = PathEffect.dashPathEffect(floatArrayOf(4.dp.toPx(), 4.dp.toPx())),
                    )
                }
            }
            for (i in colors.indices.reversed()) {
                val p = point(colors[i])
                val r = (if (i == 0) big else small) / 2 * (if (dragged == i) 1.2f else 1f)
                drawCircle(Color.Black.copy(alpha = 0.25f), r + 1.dp.toPx(), p + Offset(0f, 1.dp.toPx()))
                drawCircle(Color.White, r, p)
                drawCircle(colors[i].color, r - (if (i == 0) 3f else 2.5f).dp.toPx(), p)
            }
        }
    }
}

/** WCAG contrast ratio of two opaque colors. */
private fun contrast(a: Color, b: Color): Float {
    val la = a.luminance()
    val lb = b.luminance()
    return (maxOf(la, lb) + 0.05f) / (minOf(la, lb) + 0.05f)
}
