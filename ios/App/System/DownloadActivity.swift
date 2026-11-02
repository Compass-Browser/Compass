// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import ActivityKit
import Foundation
import UIKit

/// One Live Activity per running download: started with it, updated at
/// most once a second (or at every 5%), ended a while after it finishes.
@MainActor
final class DownloadActivities {
  private var activities: [UUID: Activity<DownloadActivityAttributes>] = [:]
  private var lastSent: [UUID: (Date, Double)] = [:]
  private var observer: NSObjectProtocol?

  init() {
    // WebKit's downloads stop with the app: say so on the Lock Screen.
    observer = NotificationCenter.default.addObserver(
      forName: UIApplication.didEnterBackgroundNotification, object: nil, queue: .main
    ) { [weak self] _ in
      MainActor.assumeIsolated { self?.pauseAll() }
    }
  }

  func update(_ item: Downloads.Item) {
    guard ActivityAuthorizationInfo().areActivitiesEnabled else { return }
    let phase: DownloadActivityAttributes.ContentState.Phase =
      item.failed != nil ? .failed : item.file != nil ? .done : .running
    let state = DownloadActivityAttributes.ContentState(
      name: item.name, fraction: item.progress, bytes: item.bytes, phase: phase)
    guard let activity = activities[item.id] else {
      guard phase == .running else { return }
      let attributes = DownloadActivityAttributes(downloadID: item.id.uuidString)
      activities[item.id] = try? Activity.request(
        attributes: attributes, content: ActivityContent(state: state, staleDate: nil))
      lastSent[item.id] = (Date(), item.progress ?? 0)
      return
    }
    if phase == .running {
      let (date, fraction) = lastSent[item.id] ?? (.distantPast, 0)
      guard Date().timeIntervalSince(date) >= 1 || (item.progress ?? 0) - fraction >= 0.05 else { return }
      lastSent[item.id] = (Date(), item.progress ?? 0)
      send(activity) { await $0.update(ActivityContent(state: state, staleDate: nil)) }
      return
    }
    activities[item.id] = nil
    lastSent[item.id] = nil
    send(activity) {
      await $0.end(
        ActivityContent(state: state, staleDate: nil), dismissalPolicy: .after(Date().addingTimeInterval(5 * 60)))
    }
  }

  /// ActivityKit's calls are async and its Activity isn't Sendable; each
  /// call only touches the one activity, so hand it over as is.
  private func send(
    _ activity: Activity<DownloadActivityAttributes>,
    _ body: @escaping @Sendable (Activity<DownloadActivityAttributes>) async -> Void
  ) {
    nonisolated(unsafe) let activity = activity
    Task.detached { await body(activity) }
  }

  private func pauseAll() {
    for activity in activities.values {
      var state = activity.content.state
      guard state.phase == .running else { continue }
      state.phase = .paused
      let paused = state
      send(activity) { await $0.update(ActivityContent(state: paused, staleDate: nil)) }
    }
  }
}
