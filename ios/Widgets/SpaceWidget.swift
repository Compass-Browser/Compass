// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import SwiftUI
import WidgetKit

/// A space's favorites as icons and, from medium up, its Pinned entries.
struct SpaceWidget: Widget {
  var body: some WidgetConfiguration {
    AppIntentConfiguration(kind: "it.cipi1965.compass.space", intent: SpaceConfiguration.self, provider: SpaceProvider()) {
      entry in
      SpaceWidgetView(entry: entry)
        .containerBackground(for: .widget) { SpaceBackground(style: entry.space?.style) }
    }
    .configurationDisplayName("Space")
    .description("A space's favorites and pinned tabs.")
    .supportedFamilies([.systemSmall, .systemMedium, .systemLarge])
  }
}

struct SpaceWidgetView: View {
  let entry: SpaceTimelineEntry
  @Environment(\.widgetFamily) private var family

  var body: some View {
    if let space = entry.space {
      content(space)
    } else {
      EmptyWidgetView(text: "Open Compass to set up your spaces.")
    }
  }

  @ViewBuilder private func content(_ space: SpaceInfo) -> some View {
    let accent = space.style.accentColor
    // Favorites first; a space without them shows its Pinned as icons.
    let icons = space.favorites.isEmpty ? space.pinned : space.favorites
    let rows = space.favorites.isEmpty ? Array(space.pinned.dropFirst(iconCount)) : space.pinned
    VStack(alignment: .leading, spacing: family == .systemSmall ? 10 : 8) {
      SpaceHeader(space: space)
      if icons.isEmpty && rows.isEmpty {
        Text("Nothing pinned yet").font(.system(size: 12)).foregroundStyle(.secondary)
          .frame(maxWidth: .infinity, maxHeight: .infinity)
      } else {
        iconGrid(Array(icons.prefix(iconCount)), accent: accent)
        if family != .systemSmall, !rows.isEmpty {
          VStack(spacing: 0) {
            ForEach(rows.prefix(rowCount)) { item in
              EntryRow(entry: item, icons: entry.icons, accent: accent, link: .node(item.id))
            }
          }
        }
        Spacer(minLength: 0)
      }
    }
  }

  private var iconCount: Int { family == .systemSmall ? 4 : family == .systemMedium ? 6 : 12 }
  private var rowCount: Int { family == .systemMedium ? 2 : 7 }

  /// Icons in a grid: two by two on small, a row of six from medium up.
  @ViewBuilder private func iconGrid(_ items: [Entry], accent: Color) -> some View {
    let columns = family == .systemSmall ? 2 : 6
    let size: CGFloat = family == .systemSmall ? 26 : 24
    let tile: CGFloat = family == .systemSmall ? 44 : 42
    Grid(horizontalSpacing: 8, verticalSpacing: 8) {
      ForEach(Array(stride(from: 0, to: items.count, by: columns)), id: \.self) { start in
        GridRow {
          ForEach(items[start..<min(start + columns, items.count)]) { item in
            Tap(link: .node(item.id)) {
              RoundedRectangle(cornerRadius: 12, style: .continuous)
                .fill(.gray.opacity(0.14))
                .overlay { SiteIconView(entry: item, icons: entry.icons, size: size, accent: accent) }
                .frame(height: tile)
                .frame(maxWidth: .infinity)
            }
          }
          // Keep the columns even on a short last row.
          ForEach(0..<(columns - min(columns, items.count - start)), id: \.self) { _ in
            Color.clear.frame(height: tile).frame(maxWidth: .infinity)
          }
        }
      }
    }
  }
}
