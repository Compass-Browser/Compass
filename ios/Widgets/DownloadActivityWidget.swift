// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import ActivityKit
import AppIntents
import CompassShared
import SwiftUI
import WidgetKit

/// A download's Live Activity: the Lock Screen banner and the Dynamic Island.
struct DownloadActivityWidget: Widget {
  var body: some WidgetConfiguration {
    ActivityConfiguration(for: DownloadActivityAttributes.self) { context in
      LockScreenDownload(context: context)
        .padding(16)
        .activityBackgroundTint(Color(.systemBackground).opacity(0.6))
        .widgetURL(DeepLink.downloads.url)
    } dynamicIsland: { context in
      let state = context.state
      return DynamicIsland {
        DynamicIslandExpandedRegion(.leading) {
          Image(systemName: Self.symbol(state.phase)).font(.system(size: 22)).foregroundStyle(Self.tint(state.phase))
        }
        DynamicIslandExpandedRegion(.trailing) {
          if state.phase == .running {
            Button(intent: CancelDownloadIntent(downloadID: context.attributes.downloadID)) {
              Image(systemName: "xmark").font(.system(size: 13, weight: .bold))
            }
            .buttonStyle(.bordered)
            .clipShape(Circle())
          }
        }
        DynamicIslandExpandedRegion(.bottom) {
          VStack(alignment: .leading, spacing: 6) {
            Text(state.name).font(.system(size: 14, weight: .semibold)).lineLimit(1)
            DownloadProgress(state: state)
          }
        }
      } compactLeading: {
        Image(systemName: "arrow.down").font(.system(size: 12, weight: .bold)).foregroundStyle(.blue)
      } compactTrailing: {
        Ring(state: state).frame(width: 16, height: 16)
      } minimal: {
        Ring(state: state).frame(width: 16, height: 16)
      }
      .widgetURL(DeepLink.downloads.url)
    }
  }

  static func symbol(_ phase: DownloadActivityAttributes.ContentState.Phase) -> String {
    switch phase {
    case .running: "arrow.down.circle.fill"
    case .paused: "pause.circle.fill"
    case .done: "checkmark.circle.fill"
    case .failed: "exclamationmark.circle.fill"
    }
  }

  static func tint(_ phase: DownloadActivityAttributes.ContentState.Phase) -> Color {
    switch phase {
    case .running: .blue
    case .paused: .orange
    case .done: .green
    case .failed: .red
    }
  }
}

struct LockScreenDownload: View {
  let context: ActivityViewContext<DownloadActivityAttributes>

  var body: some View {
    let state = context.state
    HStack(spacing: 12) {
      Image(systemName: DownloadActivityWidget.symbol(state.phase))
        .font(.system(size: 28)).foregroundStyle(DownloadActivityWidget.tint(state.phase))
      VStack(alignment: .leading, spacing: 6) {
        Text(state.name).font(.system(size: 15, weight: .semibold)).lineLimit(1)
        DownloadProgress(state: state)
      }
      if state.phase == .running {
        Button(intent: CancelDownloadIntent(downloadID: context.attributes.downloadID)) {
          Image(systemName: "xmark").font(.system(size: 13, weight: .bold))
        }
        .buttonStyle(.bordered)
        .clipShape(Circle())
      }
    }
  }
}

struct DownloadProgress: View {
  let state: DownloadActivityAttributes.ContentState

  var body: some View {
    switch state.phase {
    case .running, .paused:
      VStack(alignment: .leading, spacing: 4) {
        if let fraction = state.fraction {
          ProgressView(value: fraction).tint(DownloadActivityWidget.tint(state.phase))
        } else {
          ProgressView(value: 0).tint(DownloadActivityWidget.tint(state.phase))
        }
        Text(detail).font(.system(size: 12)).foregroundStyle(.secondary)
      }
    case .done:
      Text("Downloaded · \(size)").font(.system(size: 12)).foregroundStyle(.secondary)
    case .failed:
      Text("Download failed").font(.system(size: 12)).foregroundStyle(.secondary)
    }
  }

  private var size: String { ByteCountFormatter.string(fromByteCount: state.bytes, countStyle: .file) }

  private var detail: String {
    let done = state.fraction.map { "\(Int(($0 * 100).rounded()))% · " } ?? ""
    return state.phase == .paused ? "Paused · open Compass to finish" : done + size
  }
}

struct Ring: View {
  let state: DownloadActivityAttributes.ContentState

  var body: some View {
    switch state.phase {
    case .done: Image(systemName: "checkmark").font(.system(size: 11, weight: .bold)).foregroundStyle(.green)
    case .failed: Image(systemName: "exclamationmark").font(.system(size: 11, weight: .bold)).foregroundStyle(.red)
    case .running, .paused:
      ZStack {
        Circle().stroke(.white.opacity(0.25), lineWidth: 2.5)
        Circle().trim(from: 0, to: state.fraction ?? 0.05)
          .stroke(DownloadActivityWidget.tint(state.phase), style: StrokeStyle(lineWidth: 2.5, lineCap: .round))
          .rotationEffect(.degrees(-90))
      }
    }
  }
}
