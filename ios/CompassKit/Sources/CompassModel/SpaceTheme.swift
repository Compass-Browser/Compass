// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

// A space's theme, as the desktop's SpaceModel has it (docs/design/spaces.md,
// "Space colors", "Gradients", "Intensity and grain", "Theme editor"): any
// color or grey, up to two more gradient colors, intensity, grain and the
// light or dark mode.

/// An opaque color, 8 bits a channel.
public struct RGB: Hashable, Sendable, Codable {
  public var red: UInt8
  public var green: UInt8
  public var blue: UInt8

  public init(_ red: UInt8, _ green: UInt8, _ blue: UInt8) {
    self.red = red
    self.green = green
    self.blue = blue
  }

  /// From 0xRRGGBB.
  public init(hex: UInt32) {
    self.init(UInt8((hex >> 16) & 0xFF), UInt8((hex >> 8) & 0xFF), UInt8(hex & 0xFF))
  }

  /// As 0xRRGGBB.
  public var hex: UInt32 { UInt32(red) << 16 | UInt32(green) << 8 | UInt32(blue) }

  // Hue in degrees (0-360), saturation and lightness (0-1), as the
  // desktop's colors.ts.

  public struct HSL: Equatable, Sendable {
    public var hue: Double
    public var saturation: Double
    public var lightness: Double

    public init(hue: Double, saturation: Double, lightness: Double) {
      self.hue = hue
      self.saturation = saturation
      self.lightness = lightness
    }
  }

  public var hsl: HSL {
    let r = Double(red) / 255, g = Double(green) / 255, b = Double(blue) / 255
    let maxC = max(r, g, b), minC = min(r, g, b)
    let lightness = (maxC + minC) / 2
    let chroma = maxC - minC
    var hue = 0.0
    if chroma > 0 {
      hue =
        maxC == r
        ? ((g - b) / chroma + 6).truncatingRemainder(dividingBy: 6)
        : maxC == g ? (b - r) / chroma + 2 : (r - g) / chroma + 4
    }
    let saturation = chroma > 0 ? chroma / (1 - abs(2 * lightness - 1)) : 0
    return HSL(hue: hue * 60, saturation: min(saturation, 1), lightness: lightness)
  }

  public init(hsl: HSL) {
    let h = (hsl.hue.truncatingRemainder(dividingBy: 360) + 360).truncatingRemainder(dividingBy: 360)
    let s = min(max(hsl.saturation, 0), 1)
    let l = min(max(hsl.lightness, 0), 1)
    let c = (1 - abs(2 * l - 1)) * s
    let x = c * (1 - abs((h / 60).truncatingRemainder(dividingBy: 2) - 1))
    let m = l - c / 2
    let (r, g, b): (Double, Double, Double) =
      h < 60 ? (c, x, 0) : h < 120 ? (x, c, 0) : h < 180 ? (0, c, x) : h < 240 ? (0, x, c) : h < 300 ? (x, 0, c) : (c, 0, x)
    func byte(_ v: Double) -> UInt8 { UInt8(min(max(((v + m) * 255).rounded(), 0), 255)) }
    self.init(byte(r), byte(g), byte(b))
  }

  /// With its hue turned `degrees`.
  public func rotated(by degrees: Double) -> RGB {
    var hsl = hsl
    hsl.hue += degrees
    return RGB(hsl: hsl)
  }

  /// The blend of two colors, `t` of the way to `other`.
  public func mixed(with other: RGB, _ t: Double) -> RGB {
    func mix(_ a: UInt8, _ b: UInt8) -> UInt8 { UInt8((Double(a) + (Double(b) - Double(a)) * t).rounded()) }
    return RGB(mix(red, other.red), mix(green, other.green), mix(blue, other.blue))
  }
}

/// The palette: compass_spaces_specifics.proto's color indexes. The desktop's
/// colors (space_model.h, resources/sidebar/colors.ts); grey isn't a color.
public enum SpaceColor: Int32, Sendable, CaseIterable {
  case grey = 1, blue, purple, pink, red, orange, yellow, green

  public var rgb: RGB? {
    switch self {
    case .grey: nil
    case .blue: RGB(59, 130, 246)
    case .purple: RGB(139, 92, 246)
    case .pink: RGB(236, 72, 153)
    case .red: RGB(239, 68, 68)
    case .orange: RGB(249, 115, 22)
    case .yellow: RGB(234, 179, 8)
    case .green: RGB(34, 197, 94)
    }
  }

  /// The palette color that is exactly `rgb`, if any.
  public init?(exactly rgb: RGB) {
    guard let match = Self.allCases.first(where: { $0.rgb == rgb }) else { return nil }
    self = match
  }

  /// The palette color closest to `rgb`, grey aside: what clients that only
  /// know the palette index show (the desktop's NearestPaletteIndex()).
  public static func nearest(to rgb: RGB) -> SpaceColor {
    var best = SpaceColor.blue
    var bestDistance = Int.max
    for color in allCases {
      guard let c = color.rgb else { continue }
      let dr = Int(c.red) - Int(rgb.red), dg = Int(c.green) - Int(rgb.green), db = Int(c.blue) - Int(rgb.blue)
      let distance = dr * dr + dg * dg + db * db
      if distance < bestDistance {
        bestDistance = distance
        best = color
      }
    }
    return best
  }
}

/// Whether the space's chrome follows the system's light or dark mode or
/// keeps one; proto values (1 auto, 2 light, 3 dark).
public enum SpaceAppearance: Int32, Sendable, CaseIterable, Codable {
  case auto = 1, light, dark
}

public struct SpaceTheme: Hashable, Sendable {
  /// A space's color and its gradient colors, at most.
  public static let maxColors = 3
  public static let defaultIntensity = 0.31

  /// nil for grey (no tint).
  public var color: RGB? {
    didSet { if color == nil { gradient = [] } }
  }
  /// The colors after `color`; none for a plain color or grey.
  public var gradient: [RGB] {
    didSet {
      if color == nil, !gradient.isEmpty { gradient = [] }
      if gradient.count > Self.maxColors - 1 { gradient = Array(gradient.prefix(Self.maxColors - 1)) }
    }
  }
  /// How strongly the color tints, 0 to 1.
  public var intensity: Double {
    didSet { intensity = Self.clamp(intensity, Self.defaultIntensity) }
  }
  /// How much grain the tint has, 0 to 1.
  public var grain: Double {
    didSet { grain = Self.clamp(grain, 0) }
  }
  public var appearance: SpaceAppearance

  public init(
    color: RGB?, gradient: [RGB] = [], intensity: Double = SpaceTheme.defaultIntensity, grain: Double = 0,
    appearance: SpaceAppearance = .auto
  ) {
    self.color = color
    self.gradient = color == nil ? [] : Array(gradient.prefix(Self.maxColors - 1))
    self.intensity = Self.clamp(intensity, Self.defaultIntensity)
    self.grain = Self.clamp(grain, 0)
    self.appearance = appearance
  }

  public init(_ palette: SpaceColor) {
    self.init(color: palette.rgb)
  }

  public static let blue = SpaceTheme(.blue)

  /// The color and the gradient, in order; empty for grey.
  public var colors: [RGB] { color.map { [$0] + gradient } ?? [] }

  /// The palette color it is, if it's a plain palette color.
  public var palette: SpaceColor? {
    guard gradient.isEmpty else { return nil }
    guard let color else { return .grey }
    return SpaceColor(exactly: color)
  }

  /// Where the tint starts, is halfway and ends, evenly spaced (the
  /// desktop's SidebarTint::ToLook()): the same three times for a plain
  /// color, the two colors' blend in the middle for two.
  public var stops: [RGB] {
    let colors = colors
    guard !colors.isEmpty else { return [] }
    return (0..<Self.maxColors).map { i in
      let at = Double(i) / Double(Self.maxColors - 1) * Double(colors.count - 1)
      let before = Int(at)
      let after = min(before + 1, colors.count - 1)
      return colors[before].mixed(with: colors[after], at - Double(before))
    }
  }

  private static func clamp(_ value: Double, _ fallback: Double) -> Double {
    value.isNaN ? fallback : min(max(value, 0), 1)
  }
}

/// The theme editor's whole gradients (the desktop's GRADIENT_PRESETS).
public struct GradientPreset: Sendable, Identifiable {
  public let name: String
  public let colors: [RGB]
  public var id: String { name }

  public static let all: [GradientPreset] = [
    GradientPreset(name: "Lagoon", colors: [RGB(34, 197, 160), RGB(56, 189, 248)]),
    GradientPreset(name: "Dusk", colors: [RGB(139, 92, 246), RGB(236, 72, 153)]),
    GradientPreset(name: "Ember", colors: [RGB(239, 68, 68), RGB(249, 115, 22), RGB(234, 179, 8)]),
    GradientPreset(name: "Meadow", colors: [RGB(132, 204, 22), RGB(34, 197, 94)]),
    GradientPreset(name: "Ocean", colors: [RGB(30, 64, 175), RGB(59, 130, 246), RGB(34, 211, 238)]),
    GradientPreset(name: "Peach", colors: [RGB(251, 146, 60), RGB(244, 114, 182)]),
    GradientPreset(name: "Aurora", colors: [RGB(34, 197, 94), RGB(6, 182, 212), RGB(168, 85, 247)]),
    GradientPreset(name: "Sand", colors: [RGB(214, 188, 140), RGB(190, 150, 110)]),
    GradientPreset(name: "Slate", colors: [RGB(71, 85, 105), RGB(100, 116, 139)]),
  ]
}
