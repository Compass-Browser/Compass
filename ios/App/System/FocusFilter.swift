// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import AppIntents
import CompassShared
import Foundation

/// Settings → Focus → (a Focus) → Focus Filters → Compass: the space that
/// Focus shows. iOS runs `perform()` as the Focus comes on, and again with
/// the defaults (no space) as it goes off. The setting lands in the App
/// Group, where the app (SystemBridge.readFocus) and the widgets read it.
struct CompassFocusFilter: SetFocusFilterIntent {
  static let title: LocalizedStringResource = "Set Space"
  static let description = IntentDescription("Shows a space of Compass while this Focus is on.")

  @Parameter(title: "Space") var space: SpaceEntity?
  @Parameter(title: "Show Only This Space", default: false) var onlyThisSpace: Bool

  var displayRepresentation: DisplayRepresentation {
    guard let space else { return DisplayRepresentation(title: "Compass") }
    return DisplayRepresentation(
      title: "\(space.name)", subtitle: onlyThisSpace ? "Only this space" : "Shown first")
  }

  func perform() async throws -> some IntentResult {
    let container = SharedContainer.current
    try container.write(space.map { FocusSetting(spaceID: $0.id, onlyThisSpace: onlyThisSpace) })
    SharedContainer.postChanged()
    return .result()
  }
}
