// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package it.cipi1965.compass.shell

import android.graphics.Bitmap
import androidx.annotation.StringRes
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import it.cipi1965.compass.R
import kotlin.math.abs
import kotlin.math.pow
import kotlin.math.roundToInt

/**
 * A space's theme, as the desktop's SpaceModel has it (docs/design/spaces.md, "Space colors",
 * "Gradients", "Intensity and grain", "Theme editor"), ported from the iOS app's SpaceTheme: any
 * color or grey, up to two more gradient colors, intensity, grain and the light or dark mode.
 */
class SpaceTheme(
    /** The color then the gradient; empty for grey. At most [MAX_COLORS]. */
    colors: List<Rgb> = emptyList(),
    intensity: Double = DEFAULT_INTENSITY,
    grain: Double = 0.0,
    @JvmField val appearance: Appearance = Appearance.AUTO,
) {
    enum class Appearance(@StringRes val label: Int) {
        AUTO(R.string.compass_theme_automatic),
        LIGHT(R.string.compass_theme_light),
        DARK(R.string.compass_theme_dark),
    }

    @JvmField val colors: List<Rgb> = colors.take(MAX_COLORS)
    @JvmField val intensity: Double = clamp(intensity, DEFAULT_INTENSITY)
    @JvmField val grain: Double = clamp(grain, 0.0)

    val color: Rgb? get() = colors.firstOrNull()
    val gradient: List<Rgb> get() = colors.drop(1)
    val isGrey: Boolean get() = colors.isEmpty()

    fun copy(
        colors: List<Rgb> = this.colors,
        intensity: Double = this.intensity,
        grain: Double = this.grain,
        appearance: Appearance = this.appearance,
    ) = SpaceTheme(colors, intensity, grain, appearance)

    /** For the bridge: "#rrggbb" comma-separated. */
    fun colorsCsv(): String = colors.joinToString(",") { it.hex }

    /** The palette color it is, if it's a plain palette color ("" for grey). */
    val palette: String?
        get() = when {
            colors.isEmpty() -> ""
            colors.size == 1 -> PALETTE.firstOrNull { it == colors[0].hex }
            else -> null
        }

    /**
     * Where the tint starts, is halfway and ends, evenly spaced (the desktop's
     * SidebarTint::ToLook()): the same three times for a plain color, the two colors' blend in the
     * middle for two.
     */
    val stops: List<Rgb>
        get() {
            if (colors.isEmpty()) return emptyList()
            return (0 until MAX_COLORS).map { i ->
                val at = i.toDouble() / (MAX_COLORS - 1) * (colors.size - 1)
                val before = at.toInt()
                val after = minOf(before + 1, colors.size - 1)
                colors[before].mixed(colors[after], at - before)
            }
        }

    /** The tint's opacity: the default intensity gives about the 34% the app had before themes. */
    val alpha: Float get() = minOf(intensity * MAX_OPACITY, 1.0).toFloat()

    /**
     * Whether its chrome is dark; null follows the system. In automatic mode a tint strong enough
     * to decide how light the background is, whatever the system's mode, picks it (the desktop's
     * GetTintColorMode()).
     */
    val dark: Boolean?
        get() = when (appearance) {
            Appearance.LIGHT -> false
            Appearance.DARK -> true
            Appearance.AUTO -> {
                val stops = stops
                if (stops.isEmpty()) {
                    null
                } else {
                    val luma = stops.map { it.tint.luma }.average()
                    val overLight = alpha * luma + (1 - alpha)
                    val overDark = alpha * luma
                    when {
                        overLight > 0.5 && overDark > 0.5 -> false
                        overLight <= 0.5 && overDark <= 0.5 -> true
                        else -> null
                    }
                }
            }
        }

    /**
     * Whether text reads best light over the tint laid on a dark (|darkBase|) or light
     * background: dark text when the blend contrasts more with black than with white (WCAG
     * ratios), light text otherwise (the desktop's GetTintTextMode()).
     */
    fun darkText(darkBase: Boolean): Boolean {
        val stops = stops
        if (stops.isEmpty()) return darkBase
        val backdrop = if (darkBase) 0.0 else 1.0
        fun blend(channel: (Rgb) -> Int): Double {
            val tint = stops.map { channel(it.tint) / 255.0 }.average()
            return alpha * tint + (1 - alpha) * backdrop
        }
        fun linear(c: Double) = if (c <= 0.04045) c / 12.92 else ((c + 0.055) / 1.055).pow(2.4)
        val l = 0.2126 * linear(blend { it.red }) + 0.7152 * linear(blend { it.green }) +
            0.0722 * linear(blend { it.blue })
        val againstBlack = (l + 0.05) / 0.05
        val againstWhite = 1.05 / (l + 0.05)
        // "Dark" chrome is light text.
        return againstBlack < againstWhite
    }

    /** Controls' color: the space's color, or a neutral grey. */
    val accent: Color get() = color?.color ?: CompassStyle.greyAccent
    /** The accent as ARGB, for Java. */
    val accentArgb: Int get() = accent.toArgb()

    override fun equals(other: Any?) =
        other is SpaceTheme && colors == other.colors && intensity == other.intensity &&
            grain == other.grain && appearance == other.appearance

    override fun hashCode() = listOf(colors, intensity, grain, appearance).hashCode()

    companion object {
        const val MAX_COLORS = 3
        const val DEFAULT_INTENSITY = 0.31
        /** The tint's opacity at full intensity. */
        const val MAX_OPACITY = 1.1
        /** Grain at full strength, the desktop's: about the noise of Arc's full grain. */
        const val MAX_GRAIN = 0.115f

        @JvmField val GREY = SpaceTheme()

        /** The palette (space_model.h, colors.ts), grey ("") first. */
        @JvmField
        val PALETTE = listOf(
            "", "#3b82f6", "#8b5cf6", "#ec4899", "#ef4444", "#f97316", "#eab308", "#22c55e",
        )
        /** PALETTE's names, for accessibility. */
        val PALETTE_NAMES = listOf(
            R.string.compass_theme_grey, R.string.compass_theme_blue, R.string.compass_theme_purple,
            R.string.compass_theme_pink, R.string.compass_theme_red, R.string.compass_theme_orange,
            R.string.compass_theme_yellow, R.string.compass_theme_green,
        )

        /** The theme editor's whole gradients (the desktop's GRADIENT_PRESETS), by their names' ids. */
        val PRESETS: List<Pair<Int, List<Rgb>>> = listOf(
            R.string.compass_theme_lagoon to listOf(Rgb(34, 197, 160), Rgb(56, 189, 248)),
            R.string.compass_theme_dusk to listOf(Rgb(139, 92, 246), Rgb(236, 72, 153)),
            R.string.compass_theme_ember to listOf(Rgb(239, 68, 68), Rgb(249, 115, 22), Rgb(234, 179, 8)),
            R.string.compass_theme_meadow to listOf(Rgb(132, 204, 22), Rgb(34, 197, 94)),
            R.string.compass_theme_ocean to listOf(Rgb(30, 64, 175), Rgb(59, 130, 246), Rgb(34, 211, 238)),
            R.string.compass_theme_peach to listOf(Rgb(251, 146, 60), Rgb(244, 114, 182)),
            R.string.compass_theme_aurora to listOf(Rgb(34, 197, 94), Rgb(6, 182, 212), Rgb(168, 85, 247)),
            R.string.compass_theme_sand to listOf(Rgb(214, 188, 140), Rgb(190, 150, 110)),
            R.string.compass_theme_slate to listOf(Rgb(71, 85, 105), Rgb(100, 116, 139)),
        )

        private fun clamp(value: Double, fallback: Double) =
            if (value.isNaN()) fallback else value.coerceIn(0.0, 1.0)

        /** From the snapshot's "colors" (or the older "tint"), "intensity", "grain", "appearance". */
        fun fromJson(json: org.json.JSONObject): SpaceTheme {
            val list = json.optJSONArray("colors")
            val colors = buildList {
                if (list != null) {
                    for (i in 0 until list.length()) Rgb.parse(list.optString(i))?.let(::add)
                } else {
                    Rgb.parse(json.optString("tint"))?.let(::add)
                }
            }
            val appearance = Appearance.entries.getOrElse(json.optInt("appearance")) { Appearance.AUTO }
            return SpaceTheme(
                colors,
                json.optDouble("intensity", DEFAULT_INTENSITY),
                json.optDouble("grain", 0.0),
                appearance,
            )
        }
    }
}

/** An opaque color, 8 bits a channel (the iOS app's RGB). */
data class Rgb(val red: Int, val green: Int, val blue: Int) {
    /** Hue in degrees (0-360), saturation and lightness (0-1), as the desktop's colors.ts. */
    data class Hsl(val hue: Double, val saturation: Double, val lightness: Double)

    val color: Color get() = Color(red, green, blue)
    val hex: String get() = "#%02x%02x%02x".format(red, green, blue)

    /** How light it is, 0 to 1. */
    val luma: Double get() = (0.299 * red + 0.587 * green + 0.114 * blue) / 255

    /** The color the tint paints: duller and a little darker, as Arc paints its spaces. */
    val tint: Rgb
        get() {
            val l = luma * 255
            fun channel(v: Int) = (0.96 * (l + 0.72 * (v - l))).roundToInt().coerceIn(0, 255)
            return Rgb(channel(red), channel(green), channel(blue))
        }

    val hsl: Hsl
        get() {
            val r = red / 255.0
            val g = green / 255.0
            val b = blue / 255.0
            val max = maxOf(r, g, b)
            val min = minOf(r, g, b)
            val lightness = (max + min) / 2
            val chroma = max - min
            var hue = 0.0
            if (chroma > 0) {
                hue = when (max) {
                    r -> ((g - b) / chroma + 6) % 6
                    g -> (b - r) / chroma + 2
                    else -> (r - g) / chroma + 4
                }
            }
            val saturation = if (chroma > 0) chroma / (1 - abs(2 * lightness - 1)) else 0.0
            return Hsl(hue * 60, minOf(saturation, 1.0), lightness)
        }

    fun rotated(degrees: Double): Rgb = hsl.let { fromHsl(it.copy(hue = it.hue + degrees)) }

    /** The blend of two colors, |t| of the way to |other|. */
    fun mixed(other: Rgb, t: Double): Rgb {
        fun mix(a: Int, b: Int) = (a + (b - a) * t).roundToInt()
        return Rgb(mix(red, other.red), mix(green, other.green), mix(blue, other.blue))
    }

    companion object {
        fun parse(hex: String): Rgb? {
            if (hex.length != 7 || hex[0] != '#') return null
            val v = hex.substring(1).toIntOrNull(16) ?: return null
            return Rgb((v shr 16) and 0xff, (v shr 8) and 0xff, v and 0xff)
        }

        fun of(color: Color) = Rgb(
            (color.red * 255).roundToInt(), (color.green * 255).roundToInt(), (color.blue * 255).roundToInt(),
        )

        fun fromHsl(hsl: Hsl): Rgb {
            val h = ((hsl.hue % 360) + 360) % 360
            val s = hsl.saturation.coerceIn(0.0, 1.0)
            val l = hsl.lightness.coerceIn(0.0, 1.0)
            val c = (1 - abs(2 * l - 1)) * s
            val x = c * (1 - abs((h / 60) % 2 - 1))
            val m = l - c / 2
            val (r, g, b) = when {
                h < 60 -> Triple(c, x, 0.0)
                h < 120 -> Triple(x, c, 0.0)
                h < 180 -> Triple(0.0, c, x)
                h < 240 -> Triple(0.0, x, c)
                h < 300 -> Triple(x, 0.0, c)
                else -> Triple(c, 0.0, x)
            }
            fun byte(v: Double) = ((v + m) * 255).roundToInt().coerceIn(0, 255)
            return Rgb(byte(r), byte(g), byte(b))
        }
    }
}

/** A tile of grey noise, the same every time (the desktop's GrainTile()), one grain per pixel. */
object Grain {
    val tile: ImageBitmap by lazy {
        val size = 128
        val pixels = IntArray(size * size)
        var state = 0x2545f491L
        for (i in pixels.indices) {
            state = (state * 1664525 + 1013904223) and 0xffffffffL
            val v = (state ushr 24).toInt() and 0xff
            pixels[i] = (0xff shl 24) or (v shl 16) or (v shl 8) or v
        }
        Bitmap.createBitmap(pixels, size, size, Bitmap.Config.ARGB_8888).asImageBitmap()
    }
}
