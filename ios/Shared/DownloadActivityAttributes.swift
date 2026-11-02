// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// A download on the Lock Screen and in the Dynamic Island. The app starts
// and updates it (App/System/DownloadActivity.swift); the Widgets extension
// draws it (Widgets/DownloadActivityWidget.swift).

import ActivityKit
import AppIntents
import Foundation

struct DownloadActivityAttributes: ActivityAttributes {
  struct ContentState: Codable, Hashable {
    enum Phase: String, Codable, Hashable {
      case running
      /// The app was suspended: WebKit's download waits for it.
      case paused
      /// The app was suspended, the system downloads on: no progress until
      /// the app is back or the download ends.
      case background
      case done
      case failed
    }

    var name: String
    /// 0...1; nil when the size isn't known.
    var fraction: Double?
    var bytes: Int64
    var phase: Phase
  }

  /// The download's ID in the app (Downloads.Item.id).
  var downloadID: String
}

/// Cancel, on a running download's activity.
struct CancelDownloadIntent: LiveActivityIntent {
  static let title: LocalizedStringResource = "Cancel Download"
  static let isDiscoverable = false

  @Parameter(title: "Download") var downloadID: String

  init() {}
  init(downloadID: String) { self.downloadID = downloadID }

  func perform() async throws -> some IntentResult {
    await MainActor.run { IntentRouter.cancelDownload?(downloadID) }
    return .result()
  }
}
