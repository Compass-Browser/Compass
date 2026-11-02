// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AppIntents
import CompassShared
import SwiftUI
import WidgetKit

/// Search, a new tab and the shown space, on the Home Screen; New Tab and
/// Search on the Lock Screen.
struct QuickWidget: Widget {
  var body: some WidgetConfiguration {
    StaticConfiguration(kind: "it.cipi1965.compass.quick", provider: DevicesProvider()) { entry in
      QuickWidgetView(entry: entry)
        .containerBackground(for: .widget) { SpaceBackground(style: entry.space?.style) }
    }
    .configurationDisplayName("Quick Actions")
    .description("Search, open a new tab, or go to your space.")
    .supportedFamilies([.systemSmall, .accessoryCircular, .accessoryRectangular, .accessoryInline])
  }
}

struct QuickWidgetView: View {
  let entry: SpaceTimelineEntry
  @Environment(\.widgetFamily) private var family

  var body: some View {
    switch family {
    case .accessoryCircular:
      ZStack {
        AccessoryWidgetBackground()
        Image(systemName: "plus").font(.system(size: 22, weight: .semibold))
      }
      .widgetURL(DeepLink.newTab(space: nil).url)
      .accessibilityLabel("New Tab")
    case .accessoryRectangular:
      HStack(spacing: 6) {
        Image(systemName: "magnifyingglass").font(.system(size: 15, weight: .semibold))
        VStack(alignment: .leading, spacing: 0) {
          Text("Compass").font(.headline).widgetAccentable()
          Text("Search or enter address").font(.caption).foregroundStyle(.secondary)
        }
        Spacer(minLength: 0)
      }
      .widgetURL(DeepLink.search.url)
    case .accessoryInline:
      Label("Search Compass", systemImage: "magnifyingglass").widgetURL(DeepLink.search.url)
    default:
      small
    }
  }

  private var small: some View {
    VStack(spacing: 8) {
      Button(intent: OpenLinkIntent(.search)) {
        HStack(spacing: 6) {
          Image(systemName: "magnifyingglass").font(.system(size: 13, weight: .semibold))
          Text("Search").font(.system(size: 13, weight: .medium))
          Spacer(minLength: 0)
        }
        .foregroundStyle(.secondary)
        .padding(.horizontal, 10)
        .frame(maxWidth: .infinity, minHeight: 38)
        .background(.gray.opacity(0.16), in: Capsule())
      }
      .buttonStyle(.plain)
      HStack(spacing: 8) {
        tile(link: .newTab(space: nil), label: "New Tab") {
          Image(systemName: "plus").font(.system(size: 18, weight: .semibold))
        }
        if let space = entry.space {
          tile(link: .space(space.id), label: space.name) {
            SpaceGlyphView(space: space, size: 17).foregroundStyle(space.style.accentColor)
          }
        }
      }
    }
  }

  private func tile<Icon: View>(link: DeepLink, label: String, @ViewBuilder icon: () -> Icon) -> some View {
    Button(intent: OpenLinkIntent(link)) {
      VStack(spacing: 4) {
        icon().frame(height: 22).widgetAccentable()
        Text(label).font(.system(size: 11, weight: .medium)).lineLimit(1)
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
      .background(.gray.opacity(0.14), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
    .buttonStyle(.plain)
  }
}

/// Control Center, the Lock Screen's controls and the Action button.
@available(iOS 18.0, *)
struct NewTabControl: ControlWidget {
  var body: some ControlWidgetConfiguration {
    StaticControlConfiguration(kind: "it.cipi1965.compass.new-tab") {
      ControlWidgetButton(action: NewTabIntent()) {
        Label("New Compass Tab", systemImage: "plus.square.on.square")
      }
    }
    .displayName("New Tab")
    .description("Opens Compass on a new tab.")
  }
}
