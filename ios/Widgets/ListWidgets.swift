// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import SwiftUI
import WidgetKit

/// A space's newest Today entries, to pick up where you left off.
struct TodayWidget: Widget {
  var body: some WidgetConfiguration {
    AppIntentConfiguration(kind: "it.cipi1965.compass.today", intent: SpaceConfiguration.self, provider: SpaceProvider()) {
      entry in
      TodayWidgetView(entry: entry)
        .containerBackground(for: .widget) { SpaceBackground(style: entry.space?.style) }
    }
    .configurationDisplayName("Today")
    .description("A space's most recent tabs.")
    .supportedFamilies([.systemMedium, .systemLarge])
  }
}

struct TodayWidgetView: View {
  let entry: SpaceTimelineEntry
  @Environment(\.widgetFamily) private var family

  var body: some View {
    if let space = entry.space {
      let count = family == .systemLarge ? 9 : 3
      let more = space.todayCount - min(count, space.today.count)
      VStack(alignment: .leading, spacing: 6) {
        SpaceHeader(space: space, trailing: more > 0 ? "\(more) more" : nil)
        if space.today.isEmpty {
          Text("No tabs today").font(.system(size: 12)).foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
          VStack(spacing: 0) {
            ForEach(space.today.prefix(count)) { item in
              EntryRow(entry: item, icons: entry.icons, accent: space.style.accentColor, link: .node(item.id))
            }
            // Rows keep their height when there are few.
            ForEach(0..<max(0, count - space.today.count), id: \.self) { _ in Color.clear }
          }
        }
      }
    } else {
      EmptyWidgetView(text: "Open Compass to set up your spaces.")
    }
  }
}

/// The tabs open on your other devices (synced sessions).
struct DevicesWidget: Widget {
  var body: some WidgetConfiguration {
    StaticConfiguration(kind: "it.cipi1965.compass.devices", provider: DevicesProvider()) { entry in
      DevicesWidgetView(entry: entry)
        .containerBackground(for: .widget) { Color(.systemBackground) }
    }
    .configurationDisplayName("Other Devices")
    .description("The tabs open on your Mac and other devices.")
    .supportedFamilies([.systemMedium, .systemLarge])
  }
}

struct DevicesProvider: TimelineProvider {
  func placeholder(in context: Context) -> SpaceTimelineEntry { SpaceProvider.entry(nil, sample: true) }

  func getSnapshot(in context: Context, completion: @escaping (SpaceTimelineEntry) -> Void) {
    completion(SpaceProvider.entry(nil, sample: context.isPreview && SharedContainer.current.readSnapshot() == nil))
  }

  func getTimeline(in context: Context, completion: @escaping (Timeline<SpaceTimelineEntry>) -> Void) {
    completion(Timeline(entries: [SpaceProvider.entry(nil, sample: false)], policy: .never))
  }
}

struct DevicesWidgetView: View {
  let entry: SpaceTimelineEntry
  @Environment(\.widgetFamily) private var family

  var body: some View {
    let devices = entry.snapshot?.devices ?? []
    if devices.isEmpty {
      EmptyWidgetView(
        text: entry.snapshot?.syncOn == true
          ? "No tabs open on your other devices." : "Turn on sync in Compass to see your other devices' tabs.")
    } else {
      let rows = family == .systemLarge ? 10 : 4
      VStack(alignment: .leading, spacing: 2) {
        ForEach(Self.sections(devices, rows: rows), id: \.device.id) { section in
          HStack(spacing: 5) {
            Image(systemName: Self.symbol(section.device.kind)).font(.system(size: 11, weight: .semibold))
              .widgetAccentable()
            Text(section.device.name).font(.system(size: 12, weight: .semibold)).lineLimit(1)
            Spacer(minLength: 0)
            Text(section.device.modified, style: .relative).font(.system(size: 10)).foregroundStyle(.secondary)
          }
          .foregroundStyle(.secondary)
          .padding(.top, 2)
          ForEach(section.tabs) { tab in
            EntryRow(entry: tab, icons: entry.icons, accent: .gray, link: .open(url: tab.url, space: nil))
          }
        }
        Spacer(minLength: 0)
      }
    }
  }

  /// As many devices as fit, each with its newest tabs (at least two).
  static func sections(_ devices: [DeviceTabs], rows: Int) -> [(device: DeviceTabs, tabs: [Entry])] {
    var left = rows
    var out: [(DeviceTabs, [Entry])] = []
    for device in devices {
      // A header takes about a row.
      guard left >= 3 || out.isEmpty else { break }
      let take = min(device.tabs.count, max(1, out.isEmpty && devices.count == 1 ? left : min(left - 1, 4)))
      out.append((device, Array(device.tabs.prefix(take))))
      left -= take + 1
    }
    return out
  }

  static func symbol(_ kind: String) -> String {
    switch kind {
    case "phone": "iphone"
    case "tablet": "ipad"
    default: "laptopcomputer"
    }
  }
}
