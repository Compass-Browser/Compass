// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SyncProto

// A space's theme on the wire, as the desktop's compass_spaces_sync_bridge.cc
// reads and writes it (docs/design/sync.md, "Merged with main (space
// themes)"). Each value is compared with what the entity already says,
// decoded, so an equivalent encoding (an older client's, or a stale
// `color_rgb`) isn't rewritten, and an applied remote value isn't committed
// back.

extension SyncPb_CompassSpace {
  /// The color, nil for grey; outer nil if this version can't read it.
  var themeColor: RGB?? {
    // `color_rgb` counts only while `color` still matches it: a client that
    // predates it may have picked a palette color since.
    if hasColorRgb, colorRgb <= 0xFFFFFF {
      let rgb = RGB(hex: colorRgb)
      if color == SpaceColor.nearest(to: rgb).rawValue { return .some(rgb) }
    }
    guard let palette = SpaceColor(rawValue: color) else { return .none }
    return .some(palette.rgb)
  }

  /// The gradient colors this version reads; unreadable ones are left out.
  var themeGradient: [RGB] {
    gradient.compactMap { c in
      if c.hasRgb { return c.rgb <= 0xFFFFFF ? RGB(hex: c.rgb) : nil }
      return SpaceColor(rawValue: c.palette)?.rgb
    }
  }

  /// Unknown values read as auto.
  var themeAppearance: SpaceAppearance {
    SpaceAppearance(rawValue: appearance) ?? .auto
  }

  /// The theme it gives a space that has `current`: absent fields are the
  /// defaults, an unreadable color keeps the current one.
  func theme(over current: SpaceTheme) -> SpaceTheme {
    SpaceTheme(
      color: themeColor ?? current.color, gradient: themeGradient,
      intensity: hasIntensity ? intensity : SpaceTheme.defaultIntensity, grain: hasGrain ? grain : 0,
      appearance: themeAppearance)
  }

  /// Writes what differs from `theme`; defaults are left out.
  mutating func setTheme(_ theme: SpaceTheme) {
    if themeColor != .some(theme.color) {
      if let rgb = theme.color, SpaceColor(exactly: rgb) == nil {
        color = SpaceColor.nearest(to: rgb).rawValue
        colorRgb = rgb.hex
      } else {
        color = (theme.color.flatMap { SpaceColor(exactly: $0) } ?? .grey).rawValue
        clearColorRgb()
      }
    }
    if themeGradient != theme.gradient {
      gradient = theme.gradient.map { rgb in
        var c = SyncPb_CompassColor()
        if let palette = SpaceColor(exactly: rgb) { c.palette = palette.rawValue } else { c.rgb = rgb.hex }
        return c
      }
    }
    if (hasIntensity ? intensity : SpaceTheme.defaultIntensity) != theme.intensity {
      if theme.intensity == SpaceTheme.defaultIntensity { clearIntensity() } else { intensity = theme.intensity }
    }
    if (hasGrain ? grain : 0) != theme.grain {
      if theme.grain == 0 { clearGrain() } else { grain = theme.grain }
    }
    if themeAppearance != theme.appearance {
      if theme.appearance == .auto { clearAppearance() } else { appearance = theme.appearance.rawValue }
    }
  }
}
