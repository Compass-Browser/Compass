// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import SwiftUI

extension SpaceColor {
  var color: Color {
    switch self {
    case .grey: Color(red: 0.55, green: 0.57, blue: 0.62)
    case .blue: Color(red: 0.33, green: 0.47, blue: 0.95)
    case .purple: Color(red: 0.55, green: 0.38, blue: 0.93)
    case .pink: Color(red: 0.93, green: 0.36, blue: 0.62)
    case .red: Color(red: 0.92, green: 0.30, blue: 0.30)
    case .orange: Color(red: 0.96, green: 0.55, blue: 0.22)
    case .yellow: Color(red: 0.93, green: 0.75, blue: 0.18)
    case .green: Color(red: 0.25, green: 0.70, blue: 0.45)
    }
  }

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

/// The space's tint behind the whole sidebar, as the desktop's SidebarTint.
struct SpaceTint: View {
  let color: Color

  var body: some View {
    ZStack {
      Color(.systemBackground)
      LinearGradient(
        colors: [color.opacity(0.34), color.opacity(0.16)],
        startPoint: .top, endPoint: .bottom)
    }
    .ignoresSafeArea()
  }
}

/// A space's icon in the switcher, as on the desktop: its symbol, its emoji,
/// or its initial. The icon is `{"symbol": …}` / `{"emoji": …}` (the
/// desktop's serializer), or a bare symbol name or emoji.
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
    var value = icon.trimmingCharacters(in: .whitespaces)
    if value.hasPrefix("{"), let data = value.data(using: .utf8),
      let object = try? JSONSerialization.jsonObject(with: data) as? [String: String]
    {
      if let emoji = object["emoji"], !emoji.isEmpty { return .emoji(emoji) }
      value = object["symbol"] ?? ""
    }
    if value.isEmpty { return nil }
    if let name = symbols[value] { return .symbol(name) }
    return value.unicodeScalars.contains(where: \.properties.isEmojiPresentation) ? .emoji(value) : nil
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
