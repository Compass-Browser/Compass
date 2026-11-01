// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI

extension RGB {
  var color: Color { Color(red: Double(red) / 255, green: Double(green) / 255, blue: Double(blue) / 255) }

  init(_ color: Color) {
    let c = UIColor(color).resolvedColor(with: UITraitCollection(userInterfaceStyle: .light))
    var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
    c.getRed(&r, green: &g, blue: &b, alpha: &a)
    func byte(_ v: CGFloat) -> UInt8 { UInt8(min(max((v * 255).rounded(), 0), 255)) }
    self.init(byte(r), byte(g), byte(b))
  }
}

extension SpaceColor {
  var name: String {
    switch self {
    case .grey: "Grey"
    case .blue: "Blue"
    case .purple: "Purple"
    case .pink: "Pink"
    case .red: "Red"
    case .orange: "Orange"
    case .yellow: "Yellow"
    case .green: "Green"
    }
  }
}

extension SpaceTheme {
  /// Grey spaces' accent.
  static let greyAccent = Color(red: 0.55, green: 0.57, blue: 0.62)

  /// For icons and controls: the space's color, or a neutral grey.
  var accent: Color { color?.color ?? Self.greyAccent }

  /// The mode its chrome takes; nil follows the system.
  var colorScheme: ColorScheme? {
    switch appearance {
    case .auto: nil
    case .light: .light
    case .dark: .dark
    }
  }
}

extension View {
  /// The space's chrome in its light or dark mode (the theme editor's Mode);
  /// web pages keep the system's.
  @ViewBuilder func spaceAppearance(_ theme: SpaceTheme?) -> some View {
    if let scheme = theme?.colorScheme {
      environment(\.colorScheme, scheme)
    } else {
      self
    }
  }
}

/// A site's icon (Favicons: cached, from the page or the host), or its
/// first letter while it loads or when there's none.
struct SiteIcon: View {
  let url: String
  var size: CGFloat = 18

  var body: some View {
    let host = URL(string: url)?.host() ?? ""
    Group {
      if let image = Favicons.shared.image(for: url) {
        Image(uiImage: image).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
      } else {
        Text(host.replacingOccurrences(of: "www.", with: "").prefix(1).uppercased())
          .font(.system(size: size * 0.6, weight: .semibold))
          .frame(maxWidth: .infinity, maxHeight: .infinity)
          .background(.quaternary, in: RoundedRectangle(cornerRadius: size * 0.25))
      }
    }
    .frame(width: size, height: size)
    .clipShape(RoundedRectangle(cornerRadius: size * 0.25))
  }
}

/// What the phone can open: web pages. Entries synced from the desktop on
/// its own pages (compass://, chrome://, about:, file:…) keep syncing but
/// aren't shown here.
enum PhoneVisible {
  static func url(_ url: String) -> Bool {
    guard let scheme = URL(string: url)?.scheme?.lowercased() else { return false }
    return scheme == "http" || scheme == "https"
  }
}

extension SidebarNode {
  /// Shown on the phone: a web tab, a split with one, or a folder (folders
  /// always show, even with nothing visible inside).
  var isPhoneVisible: Bool {
    switch kind {
    case .folder: true
    case .split: children.contains { PhoneVisible.url($0.url) }
    default: isTab ? PhoneVisible.url(url) : true
    }
  }
}

extension Array where Element == SidebarNode {
  var phoneVisible: [SidebarNode] { filter(\.isPhoneVisible) }
}

/// The desktop sidebar's look (resources/sidebar/app.css), at phone sizes:
/// rows are 44 pt for touch where the desktop's are 32 px.
enum SidebarStyle {
  static let rowHeight: CGFloat = 44
  static let rowRadius: CGFloat = 10
  static let favicon: CGFloat = 18
  static let indent: CGFloat = 18
  static let tileHeight: CGFloat = 52
  static let tileRadius: CGFloat = 12
  /// `.tile` and the hover/pressed fills: grey at low opacity, so the space's
  /// tint shows through.
  static let fill = Color.gray.opacity(0.14)
  /// The shown entry: the desktop's active tab, a light pill with a shadow.
  static let active = Color(.secondarySystemGroupedBackground)
  /// Folders opening, entries coming and going.
  static let motion = Animation.snappy(duration: 0.25)
  /// A folder opening or closing: quick, its children sliding down.
  static let folder = Animation.snappy(duration: 0.22)
}

/// The space's tint behind the whole sidebar, as the desktop's SidebarTint:
/// its stops as one gradient from the top left corner to the bottom right,
/// as strong as its intensity, fading a little toward the bottom, with its
/// grain over it. Grey tints nothing.
struct SpaceTint: View {
  let theme: SpaceTheme

  /// The tint's opacity at full intensity; the default intensity (0.31)
  /// gives about the 34% the phone had before themes.
  static let maxOpacity = 1.1
  /// Grain at full strength (the desktop's is 4.4%, on a much larger tint).
  static let maxGrain = 0.09

  var body: some View {
    ZStack {
      Color(.systemBackground)
      let alpha = min(theme.intensity * Self.maxOpacity, 1)
      let stops = theme.stops
      LinearGradient(
        colors: stops.isEmpty ? [.clear, .clear] : stops.map { $0.color.opacity(alpha) },
        startPoint: .topLeading, endPoint: .bottomTrailing
      )
      .mask(LinearGradient(colors: [.white, .white.opacity(0.5)], startPoint: .top, endPoint: .bottom))
      if theme.grain > 0, !stops.isEmpty {
        Image(uiImage: Grain.tile)
          .resizable(resizingMode: .tile)
          .blendMode(.overlay)
          .opacity(theme.grain * Self.maxGrain)
          .allowsHitTesting(false)
      }
    }
    .animation(.easeOut(duration: 0.28), value: theme)
    .ignoresSafeArea()
  }
}

/// A tile of grey noise, the same every time (the desktop's GrainTile()),
/// one grain per pixel.
enum Grain {
  static let tile: UIImage = {
    let size = 128
    var pixels = [UInt8](repeating: 255, count: size * size * 4)
    var state: UInt32 = 0x2545f491
    for i in 0..<(size * size) {
      state = state &* 1664525 &+ 1013904223
      let v = UInt8(state >> 24)
      pixels[i * 4] = v
      pixels[i * 4 + 1] = v
      pixels[i * 4 + 2] = v
    }
    let provider = CGDataProvider(data: Data(pixels) as CFData)!
    let image = CGImage(
      width: size, height: size, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: size * 4,
      space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
      provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)!
    return UIImage(cgImage: image, scale: UITraitCollection.current.displayScale, orientation: .up)
  }()
}

/// The space's colors in a small circle, diagonally (the desktop menu's
/// theme chip).
struct ThemeChip: View {
  let theme: SpaceTheme
  var size: CGFloat = 22

  var body: some View {
    Circle()
      .fill(
        theme.stops.isEmpty
          ? AnyShapeStyle(SpaceTheme.greyAccent)
          : AnyShapeStyle(LinearGradient(colors: theme.stops.map(\.color), startPoint: .topLeading, endPoint: .bottomTrailing))
      )
      .overlay(Circle().stroke(Color.primary.opacity(0.12), lineWidth: 0.5))
      .frame(width: size, height: size)
  }
}

/// A space's icon in the switcher, as on the desktop: its symbol, its emoji,
/// or its initial. On the wire `icon` is "emoji:<emoji>" or
/// "symbol:<name>" (compass_spaces_specifics.proto).
struct SpaceIcon: View {
  let space: Space
  var size: CGFloat = 18

  var body: some View {
    switch Self.parse(space.icon) {
    case .symbol(let name):
      Image(systemName: name).font(.system(size: size * 0.9, weight: .semibold))
    case .emoji(let emoji):
      Text(emoji).font(.system(size: size * 0.85))
    case nil:
      Text(space.name.first.map { String($0).uppercased() } ?? "•")
        .font(.system(size: size * 0.9, weight: .bold, design: .rounded))
    }
  }

  enum Kind: Equatable {
    case symbol(String)
    case emoji(String)
  }

  static func parse(_ icon: String) -> Kind? {
    if icon.hasPrefix("emoji:") {
      let emoji = String(icon.dropFirst(6))
      return emoji.isEmpty ? nil : .emoji(emoji)
    }
    if icon.hasPrefix("symbol:") {
      return symbols[String(icon.dropFirst(7))].map(Kind.symbol)
    }
    return nil
  }

  /// The desktop's symbols (resources/sidebar/space_icons.ts) as SF Symbols.
  static let symbols: [String: String] = [
    "home": "house.fill", "work": "briefcase.fill", "person": "person.fill",
    "code": "chevron.left.forwardslash.chevron.right", "database": "cylinder.split.1x2.fill",
    "build": "hammer.fill", "graduation": "graduationcap.fill", "book": "book.fill",
    "lightbulb": "lightbulb.fill", "palette": "paintpalette.fill", "image": "photo.fill",
    "music": "music.note", "mic": "mic.fill", "video": "video.fill",
    "game": "gamecontroller.fill", "travel": "airplane", "globe": "globe",
    "compass": "safari.fill", "car": "car.fill", "shopping": "cart.fill",
    "wallet": "wallet.bifold.fill", "finance": "chart.line.uptrend.xyaxis",
    "science": "flask.fill", "gavel": "building.columns.fill", "shield": "shield.fill",
    "lock": "lock.fill", "celebration": "party.popper.fill", "bolt": "bolt.fill",
  ]
}

extension View {
  /// Floating chrome (the browser's bottom bar, banners): Liquid Glass on
  /// iOS 26 and later, a material with a soft shadow before.
  @ViewBuilder func floatingGlass<S: Shape>(in shape: S) -> some View {
    if #available(iOS 26.0, *) {
      glassEffect(.regular, in: shape)
    } else {
      background(.regularMaterial, in: shape)
        .overlay(shape.stroke(Color.primary.opacity(0.08), lineWidth: 0.5))
        .shadow(color: .black.opacity(0.12), radius: 14, y: 4)
    }
  }
}

// Cards and rows of the command bar and the page menu.

/// A light rounded card over the space's tint, its rows split by hairlines.
/// Each row draws a hairline on its top edge; the card hides the first one.
struct Card<Content: View>: View {

  @ViewBuilder var content: Content

  var body: some View {
    VStack(spacing: 0) { content }
      .padding(.top, -1)
      .background(cardFill, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
    .clipShape(RoundedRectangle(cornerRadius: 16, style: .continuous))
  }
}

/// A small uppercase title over a card, as the sidebar's section headers.
struct CardSection<Content: View>: View {
  let title: String
  @ViewBuilder var content: Content

  init(_ title: String, @ViewBuilder content: () -> Content) {
    self.title = title
    self.content = content()
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      Text(title.uppercased())
        .font(.system(size: 12, weight: .semibold))
        .tracking(0.4)
        .opacity(0.55)
        .lineLimit(1)
        .padding(.leading, 6)
      Card { content }
    }
    .padding(.top, 10)
  }
}

/// One suggestion: an icon tile, a title over a dimmed subtitle, and an
/// optional hint of what tapping does.
struct Row<Icon: View>: View {
  let title: String
  let subtitle: String
  var trailing: String? = nil
  let action: () -> Void
  @ViewBuilder var icon: Icon

  var body: some View {
    Button(action: action) {
      HStack(spacing: 12) {
        icon
        VStack(alignment: .leading, spacing: 1) {
          Text(title).font(.system(size: 16, weight: .medium)).lineLimit(1)
          Text(subtitle).font(.footnote).foregroundStyle(.secondary).lineLimit(1)
        }
        Spacer(minLength: 4)
        if let trailing {
          Image(systemName: trailing)
            .font(.system(size: 13, weight: .semibold))
            .foregroundStyle(.tertiary)
        }
      }
      .padding(.horizontal, 12)
      .frame(minHeight: 56)
      .contentShape(Rectangle())
    }
    .overlay(alignment: .top) {
      Rectangle().fill(.separator).frame(height: 1 / 3).padding(.leading, 56)
    }
    .buttonStyle(RowPressStyle())
  }
}

/// A site's favicon on a soft tile, so icons of any shape line up.
struct SiteTile: View {
  let url: String

  var body: some View {
    SiteIcon(url: url, size: 18)
      .frame(width: 32, height: 32)
      .background(Color.primary.opacity(0.06), in: RoundedRectangle(cornerRadius: 9))
  }
}

/// Rows dim slightly while pressed, without the system tint.
struct RowPressStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    configuration.label
      .foregroundStyle(.primary)
      .background(Color.primary.opacity(configuration.isPressed ? 0.08 : 0))
  }
}

/// The cards' and the search field's fill over the space's tint.
let cardFill = Color(.secondarySystemGroupedBackground).opacity(0.82)
