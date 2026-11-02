// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AppIntents
import CompassShared
import SwiftUI
import WidgetKit

// What every Compass widget shares: the snapshot the app writes, the space
// a widget shows, its colors and the site icons.

/// A widget's configuration: which space (none = the shown one, or the
/// Focus filter's while its Focus is on).
struct SpaceConfiguration: WidgetConfigurationIntent {
  static let title: LocalizedStringResource = "Space"
  static let description = IntentDescription("The space this widget shows.")

  @Parameter(title: "Space") var space: SpaceEntity?

  init() {}
}

struct SpaceTimelineEntry: TimelineEntry {
  let date: Date
  let snapshot: Snapshot?
  let space: SpaceInfo?
  let icons: SharedContainer
}

/// One entry, until the app writes a new snapshot and reloads the widgets.
struct SpaceProvider: AppIntentTimelineProvider {
  func placeholder(in context: Context) -> SpaceTimelineEntry { Self.entry(nil, sample: true) }

  func snapshot(for configuration: SpaceConfiguration, in context: Context) async -> SpaceTimelineEntry {
    Self.entry(configuration.space?.id, sample: context.isPreview && SharedContainer.current.readSnapshot() == nil)
  }

  func timeline(for configuration: SpaceConfiguration, in context: Context) async -> Timeline<SpaceTimelineEntry> {
    Timeline(entries: [Self.entry(configuration.space?.id, sample: false)], policy: .never)
  }

  static func entry(_ spaceID: String?, sample: Bool) -> SpaceTimelineEntry {
    let container = SharedContainer.current
    let snapshot = sample ? Sample.snapshot : container.readSnapshot()
    let focus = container.readFocus()?.spaceID
    let space = snapshot?.space(id: spaceID) ?? snapshot?.space(id: focus) ?? snapshot?.currentSpace
    return SpaceTimelineEntry(date: Date(), snapshot: snapshot, space: space, icons: container)
  }
}

// MARK: Look

extension Color {
  init(hex: UInt32) {
    self.init(
      red: Double((hex >> 16) & 0xFF) / 255, green: Double((hex >> 8) & 0xFF) / 255, blue: Double(hex & 0xFF) / 255)
  }
}

extension SpaceStyle {
  var accentColor: Color { Color(hex: accent) }
}

/// The space's tint, as the app's sidebar paints it.
struct SpaceBackground: View {
  let style: SpaceStyle?

  var body: some View {
    ZStack {
      Color(.systemBackground)
      if let style, !style.stops.isEmpty {
        LinearGradient(
          colors: (style.stops.count == 1 ? style.stops + style.stops : style.stops).map {
            Color(hex: $0).opacity(style.opacity)
          },
          startPoint: .topLeading, endPoint: .bottomTrailing)
      }
    }
  }
}

struct SpaceGlyphView: View {
  let space: SpaceInfo
  var size: CGFloat = 14

  var body: some View {
    switch space.icon {
    case .symbol(let name): Image(systemName: name).font(.system(size: size, weight: .semibold))
    case .emoji(let emoji): Text(emoji).font(.system(size: size))
    case .initial(let letter): Text(letter).font(.system(size: size, weight: .bold, design: .rounded))
    }
  }
}

/// A site's icon from the App Group, or its initial on the accent.
struct SiteIconView: View {
  let entry: Entry
  let icons: SharedContainer
  var size: CGFloat = 16
  var accent: Color = .gray

  var body: some View {
    if let image = UIImage(contentsOfFile: icons.icon(host: entry.iconHost).path) {
      Image(uiImage: image).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
        .frame(width: size, height: size)
        .clipShape(RoundedRectangle(cornerRadius: size * 0.22, style: .continuous))
    } else {
      RoundedRectangle(cornerRadius: size * 0.22, style: .continuous)
        .fill(accent.opacity(0.25))
        .overlay {
          Text(entry.host.first.map { String($0).uppercased() } ?? "•")
            .font(.system(size: size * 0.55, weight: .bold, design: .rounded))
            .foregroundStyle(accent)
        }
        .frame(width: size, height: size)
    }
  }
}

/// The space's name and symbol, over every space widget.
struct SpaceHeader: View {
  let space: SpaceInfo
  var trailing: String?

  var body: some View {
    Tap(link: .space(space.id)) {
      HStack(spacing: 6) {
        SpaceGlyphView(space: space, size: 12)
          .foregroundStyle(space.style.accentColor)
          .widgetAccentable()
        Text(space.name).font(.system(size: 13, weight: .semibold)).lineLimit(1)
        Spacer(minLength: 0)
        if let trailing {
          Text(trailing).font(.system(size: 11, weight: .medium)).foregroundStyle(.secondary)
        }
      }
    }
  }
}

/// A list row: icon and title.
struct EntryRow: View {
  let entry: Entry
  let icons: SharedContainer
  let accent: Color
  var link: DeepLink

  var body: some View {
    Tap(link: link) {
      HStack(spacing: 8) {
        SiteIconView(entry: entry, icons: icons, size: 16, accent: accent)
        Text(entry.label).font(.system(size: 13)).lineLimit(1)
        Spacer(minLength: 0)
      }
      .frame(maxHeight: .infinity)
    }
  }
}

/// A tap that opens a `compass:` link: a Link, or in a small widget (which
/// has no links) a button running OpenLinkIntent.
struct Tap<Label: View>: View {
  let link: DeepLink
  @ViewBuilder let label: () -> Label
  @Environment(\.widgetFamily) private var family

  var body: some View {
    if family == .systemSmall {
      Button(intent: OpenLinkIntent(link)) { label() }.buttonStyle(.plain)
    } else {
      Link(destination: link.url) { label() }
    }
  }
}

/// Before the app has written anything.
struct EmptyWidgetView: View {
  let text: String

  var body: some View {
    VStack(spacing: 6) {
      Image(systemName: "safari").font(.system(size: 22)).foregroundStyle(.secondary)
      Text(text).font(.system(size: 12)).foregroundStyle(.secondary).multilineTextAlignment(.center)
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
  }
}

// MARK: Sample (the widget gallery before the app first runs)

enum Sample {
  static let snapshot: Snapshot = {
    let entries = [
      ("Mail", "https://mail.example.com"), ("Calendar", "https://calendar.example.com"),
      ("Docs", "https://docs.example.com"), ("News", "https://news.example.com"),
      ("Music", "https://music.example.com"), ("Maps", "https://maps.example.com"),
    ].enumerated().map { Entry(id: "\($0.offset)", title: $0.element.0, url: $0.element.1) }
    let space = SpaceInfo(
      id: "sample", name: "Home", icon: .symbol("house.fill"),
      style: SpaceStyle(stops: [0x5E7FD6, 0x8E6FD0], opacity: 0.34, accent: 0x4F7BEA),
      favorites: Array(entries.prefix(4)), pinned: Array(entries.suffix(4)), today: entries.reversed(),
      todayCount: entries.count)
    let mac = DeviceTabs(id: "mac", name: "MacBook", kind: "desktop", modified: Date(), tabs: Array(entries.prefix(5)))
    return Snapshot(spaces: [space], recentSpaceIDs: ["sample"], devices: [mac], syncOn: true)
  }()
}
