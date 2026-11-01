// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Real touches on the browser's bar. These drive the gesture; the result is
/// checked from simulator screenshots taken while the finger is held
/// (tools/compass ios uitest).
final class SwipeUpTests: XCTestCase {
  /// Drag the bar halfway up and hold: the sidebar should sit under the finger.
  func testSwipeUpFollowsFinger() {
    let app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", "https://example.org/"]
    app.launch()
    sleep(5)
    let bar = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.91))
    let half = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5))
    bar.press(forDuration: 0.05, thenDragTo: half, withVelocity: .slow, thenHoldForDuration: 6)
    sleep(3)
  }
}
