// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI

/// A space's theme, edited as on the desktop (docs/design/spaces.md, "Theme
/// editor"): the mode, a pad with the space's colors as dots (hue round the
/// center, saturation out from it), one to three colors, the palette and
/// whole gradients, any color, intensity and grain. What changes shows at
/// once, here and in the sidebar behind, and goes to the store (and sync)
/// at most every 50 ms.
struct ThemeEditor: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  let space: Space

  @State private var draft: SpaceTheme
  @State private var pushing: Task<Void, Never>?

  init(space: Space) {
    self.space = space
    _draft = State(initialValue: space.theme)
  }

  var body: some View {
    NavigationStack {
      ScrollView {
        VStack(spacing: 22) {
          modePicker
          ThemePad(theme: $draft)
            .frame(maxWidth: 300)
            .padding(.horizontal, 24)
          countControls
          palette
          presets
          if draft.color != nil {
            sliders
              .transition(.opacity.combined(with: .move(edge: .bottom)))
          }
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 12)
        .animation(.snappy(duration: 0.25), value: draft.color == nil)
      }
      .background(SpaceTint(theme: draft))
      .navigationTitle(space.name)
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .confirmationAction) {
          Button("Done") { dismiss() }.accessibilityIdentifier("ThemeDone")
        }
      }
    }
    .spaceAppearance(draft)
    .presentationDetents([.large])
    .presentationDragIndicator(.visible)
    .onChange(of: draft) { push() }
    .onDisappear {
      pushing?.cancel()
      app.store.setTheme(space, draft)
    }
  }

  /// Applies the draft at most every 50 ms (the desktop's CUSTOM_COLOR_MS).
  private func push() {
    guard pushing == nil else { return }
    pushing = Task { @MainActor in
      try? await Task.sleep(for: .milliseconds(50))
      guard !Task.isCancelled else { return }
      app.store.setTheme(space, draft)
      pushing = nil
    }
  }

  // MARK: Mode

  private var modePicker: some View {
    HStack(spacing: 8) {
      ForEach(SpaceAppearance.allCases, id: \.self) { mode in
        let selected = draft.appearance == mode
        Button {
          draft.appearance = mode
        } label: {
          VStack(spacing: 4) {
            Image(systemName: mode.symbol).font(.system(size: 17, weight: .semibold))
            Text(mode.name).font(.system(size: 12, weight: .medium))
          }
          .frame(maxWidth: .infinity, minHeight: 54)
          .foregroundStyle(selected ? draft.accent : .primary)
          .background(selected ? cardFill : SidebarStyle.fill, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
          .overlay(
            RoundedRectangle(cornerRadius: 12, style: .continuous)
              .strokeBorder(draft.accent.opacity(selected ? 0.6 : 0), lineWidth: 1.5))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(mode.name)
        .accessibilityAddTraits(selected ? .isSelected : [])
      }
    }
  }

  // MARK: Colors

  /// − and + : one color less or more, 1 to 3 (the new one 40° on from the
  /// last).
  private var countControls: some View {
    HStack(spacing: 18) {
      roundButton("minus", label: "Fewer colors", enabled: !draft.gradient.isEmpty) {
        draft.gradient.removeLast()
      }
      HStack(spacing: 5) {
        ForEach(0..<SpaceTheme.maxColors, id: \.self) { i in
          Circle()
            .fill(i < draft.colors.count ? draft.colors[i].color : Color.primary.opacity(0.15))
            .frame(width: 8, height: 8)
        }
      }
      .accessibilityHidden(true)
      roundButton("plus", label: "More colors", enabled: draft.colors.count < SpaceTheme.maxColors) {
        if let last = draft.colors.last {
          draft.gradient.append(last.rotated(by: 40))
        } else {
          draft.color = SpaceColor.blue.rgb
        }
      }
    }
  }

  private func roundButton(_ symbol: String, label: String, enabled: Bool, action: @escaping () -> Void) -> some View {
    Button(action: action) {
      Image(systemName: symbol)
        .font(.system(size: 15, weight: .bold))
        .frame(width: 40, height: 40)
        .background(SidebarStyle.fill, in: Circle())
    }
    .buttonStyle(.plain)
    .disabled(!enabled)
    .opacity(enabled ? 1 : 0.35)
    .accessibilityLabel(label)
  }

  /// Grey and the eight colors, then any color. A palette color replaces
  /// the gradient; the color well keeps it.
  private var palette: some View {
    CardSection("Colors") {
      HStack(spacing: 0) {
        ForEach(SpaceColor.allCases, id: \.self) { color in
          let selected = draft.palette == color
          Button {
            draft.color = color.rgb
            draft.gradient = []
          } label: {
            Circle()
              .fill(color.rgb?.color ?? SpaceTheme.greyAccent)
              .frame(width: 26, height: 26)
              .padding(3)
              .overlay(Circle().strokeBorder(Color.primary.opacity(selected ? 0.7 : 0), lineWidth: 2))
              .frame(maxWidth: .infinity, minHeight: 44)
          }
          .buttonStyle(.plain)
          .accessibilityLabel(color.name)
          .accessibilityAddTraits(selected ? .isSelected : [])
        }
      }
      .padding(.horizontal, 6)
      .padding(.vertical, 4)
      ColorPicker(
        selection: Binding(
          get: { draft.color?.color ?? SpaceTheme.greyAccent },
          set: { draft.color = RGB($0) }),
        supportsOpacity: false
      ) {
        Text("Any Color").font(.system(size: 16, weight: .medium))
      }
      .padding(.horizontal, 14)
      .frame(minHeight: 50)
      .overlay(alignment: .top) { Rectangle().fill(.separator).frame(height: 1 / 3) }
    }
  }

  private var presets: some View {
    CardSection("Gradients") {
      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 14) {
          ForEach(GradientPreset.all) { preset in
            let selected = draft.colors == preset.colors
            Button {
              draft.color = preset.colors[0]
              draft.gradient = Array(preset.colors.dropFirst())
            } label: {
              VStack(spacing: 5) {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                  .fill(LinearGradient(
                    colors: SpaceTheme(color: preset.colors[0], gradient: Array(preset.colors.dropFirst()))
                      .stops.map(\.color),
                    startPoint: .topLeading, endPoint: .bottomTrailing))
                  .frame(width: 56, height: 38)
                  .overlay(
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                      .strokeBorder(Color.primary.opacity(selected ? 0.7 : 0), lineWidth: 2))
                Text(preset.name).font(.system(size: 11, weight: .medium)).opacity(0.7)
              }
            }
            .buttonStyle(.plain)
            .accessibilityLabel(preset.name)
          }
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 12)
      }
    }
  }

  // MARK: Intensity and grain

  private var sliders: some View {
    CardSection("Look") {
      sliderRow("Intensity", symbol: "circle.lefthalf.filled", value: $draft.intensity)
      sliderRow("Grain", symbol: "circle.dotted", value: $draft.grain)
        .overlay(alignment: .top) { Rectangle().fill(.separator).frame(height: 1 / 3) }
    }
  }

  private func sliderRow(_ title: String, symbol: String, value: Binding<Double>) -> some View {
    HStack(spacing: 12) {
      Image(systemName: symbol).frame(width: 22).opacity(0.7)
      Text(title).font(.system(size: 16, weight: .medium)).frame(width: 78, alignment: .leading)
      Slider(value: value, in: 0...1).tint(draft.accent)
        .accessibilityLabel(title)
      Text("\(Int((value.wrappedValue * 100).rounded()))")
        .font(.system(size: 13, weight: .medium).monospacedDigit())
        .opacity(0.55)
        .frame(width: 30, alignment: .trailing)
    }
    .padding(.horizontal, 14)
    .frame(minHeight: 52)
  }
}

extension SpaceAppearance {
  var name: String {
    switch self {
    case .auto: "Automatic"
    case .light: "Light"
    case .dark: "Dark"
    }
  }

  var symbol: String {
    switch self {
    case .auto: "sparkles"
    case .light: "sun.max.fill"
    case .dark: "moon.fill"
    }
  }
}

/// The pad: the theme's colors as dots on a hue wheel, hue as the angle
/// round the center (red at the top, clockwise), saturation as the distance.
/// The big dot is the color, the small ones the gradient. Dragging a dot
/// keeps its lightness; dragging the big one turns and pushes the others by
/// as much, so the gradient keeps its shape. A touch away from the dots
/// moves the big one there; on a grey space it makes the first color, at
/// 55% lightness.
struct ThemePad: View {
  @Binding var theme: SpaceTheme
  /// The dot being dragged and where the drag started.
  @State private var dragged: (index: Int, start: [RGB])?

  private static let big: CGFloat = 34
  private static let small: CGFloat = 24

  var body: some View {
    GeometryReader { geo in
      let side = min(geo.size.width, geo.size.height)
      let radius = side / 2
      ZStack {
        Circle()
          .fill(AngularGradient(
            colors: stride(from: 0.0, through: 360, by: 30).map { RGB(hsl: .init(hue: $0, saturation: 0.75, lightness: 0.6)).color },
            center: .center, startAngle: .degrees(-90), endAngle: .degrees(270)))
          .overlay(Circle().fill(RadialGradient(
            colors: [Color(.systemBackground), Color(.systemBackground).opacity(0)],
            center: .center, startRadius: 0, endRadius: radius)))
          .opacity(0.55)
          .overlay(Circle().strokeBorder(Color.primary.opacity(0.08), lineWidth: 1))
        // The gradient's path, from the color through the others.
        if theme.colors.count > 1 {
          Path { p in
            p.addLines(theme.colors.map { point(for: $0, radius: radius) })
          }
          .stroke(Color.primary.opacity(0.35), style: StrokeStyle(lineWidth: 1.5, dash: [4, 4]))
        }
        ForEach(Array(theme.colors.enumerated()).reversed(), id: \.offset) { i, color in
          let size = i == 0 ? Self.big : Self.small
          Circle()
            .fill(color.color)
            .overlay(Circle().strokeBorder(.white, lineWidth: i == 0 ? 3 : 2.5))
            .shadow(color: .black.opacity(0.25), radius: 4, y: 1)
            .frame(width: size, height: size)
            .scaleEffect(dragged?.index == i ? 1.2 : 1)
            .position(point(for: color, radius: radius))
            .accessibilityElement()
            .accessibilityLabel(i == 0 ? "Color" : "Gradient color \(i)")
            .accessibilityValue("Hue \(Int(color.hsl.hue)), saturation \(Int(color.hsl.saturation * 100))%")
            .accessibilityAdjustableAction { direction in
              nudge(i, hue: direction == .increment ? 5 : -5)
            }
        }
      }
      .frame(width: side, height: side)
      .contentShape(Circle())
      .gesture(drag(radius: radius))
      .animation(.snappy(duration: 0.15), value: dragged?.index)
      .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
    .aspectRatio(1, contentMode: .fit)
    .accessibilityElement(children: .contain)
    .accessibilityIdentifier("ThemePad")
  }

  private func point(for color: RGB, radius: CGFloat) -> CGPoint {
    let hsl = color.hsl
    let angle = hsl.hue * .pi / 180
    let distance = hsl.saturation * (radius - Self.big / 2)
    return CGPoint(x: radius + sin(angle) * distance, y: radius - cos(angle) * distance)
  }

  /// Hue and saturation at a point of the pad.
  private func hueSaturation(at p: CGPoint, radius: CGFloat) -> (hue: Double, saturation: Double) {
    let dx = p.x - radius, dy = p.y - radius
    let hue = (atan2(dx, -dy) * 180 / .pi + 360).truncatingRemainder(dividingBy: 360)
    let saturation = min(hypot(dx, dy) / (radius - Self.big / 2), 1)
    return (hue, saturation)
  }

  private func drag(radius: CGFloat) -> some Gesture {
    DragGesture(minimumDistance: 0)
      .onChanged { value in
        if dragged == nil {
          let colors = theme.colors
          // The dot under the finger, small ones first (they're drawn on top).
          let hit = colors.indices.reversed().first { i in
            let c = point(for: colors[i], radius: radius)
            return hypot(c.x - value.startLocation.x, c.y - value.startLocation.y) < 26
          }
          if let hit {
            dragged = (hit, colors)
          } else {
            // Away from the dots: the big dot jumps there.
            if theme.color == nil {
              theme.color = RGB(hsl: .init(hue: 0, saturation: 0, lightness: 0.55))
            }
            dragged = (0, theme.colors)
            move(0, from: theme.colors, to: value.startLocation, radius: radius)
          }
        }
        guard let dragged else { return }
        move(dragged.index, from: dragged.start, to: value.location, radius: radius)
      }
      .onEnded { _ in dragged = nil }
  }

  /// Puts dot `index` at `location`, keeping its lightness; the big one
  /// takes the others along by as much hue and saturation.
  private func move(_ index: Int, from start: [RGB], to location: CGPoint, radius: CGFloat) {
    guard start.indices.contains(index) else { return }
    let (hue, saturation) = hueSaturation(at: location, radius: radius)
    let was = start[index].hsl
    var colors = start
    colors[index] = RGB(hsl: .init(hue: hue, saturation: saturation, lightness: was.lightness))
    if index == 0 {
      let dHue = hue - was.hue, dSat = saturation - was.saturation
      for i in colors.indices.dropFirst() {
        var hsl = start[i].hsl
        hsl.hue += dHue
        hsl.saturation += dSat
        colors[i] = RGB(hsl: hsl)
      }
    }
    theme.color = colors[0]
    theme.gradient = Array(colors.dropFirst())
  }

  private func nudge(_ index: Int, hue: Double) {
    var colors = theme.colors
    guard colors.indices.contains(index) else { return }
    colors[index] = colors[index].rotated(by: hue)
    theme.color = colors[0]
    theme.gradient = Array(colors.dropFirst())
  }
}
