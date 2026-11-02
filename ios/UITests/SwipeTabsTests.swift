// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Swiping the bar sideways moves between the space's entries, Safari-style.
final class SwipeTabsTests: XCTestCase {
  private func address(_ app: XCUIApplication, is host: String) -> Bool {
    for _ in 0..<30 {
      if app.buttons["Address"].value as? String == host { return true }
      usleep(100_000)
    }
    return false
  }

  private func swipeBar(_ app: XCUIApplication, from: CGFloat, to: CGFloat, hold: TimeInterval = 0) {
    let start = app.coordinate(withNormalizedOffset: CGVector(dx: from, dy: 0.91))
    let end = app.coordinate(withNormalizedOffset: CGVector(dx: to, dy: 0.91))
    start.press(forDuration: 0.05, thenDragTo: end, withVelocity: .slow, thenHoldForDuration: hold)
  }

  /// Left goes to the older entry, right comes back; a short swipe springs
  /// back to the same one.
  func testSwipeChangesTab() {
    let app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURLs", "https://example.org/ https://example.com/"]
    app.launch()
    XCTAssertTrue(address(app, is: "example.com"))
    swipeBar(app, from: 0.5, to: 0.42)
    XCTAssertTrue(address(app, is: "example.com"))
    swipeBar(app, from: 0.85, to: 0.15)
    XCTAssertTrue(address(app, is: "example.org"))
    swipeBar(app, from: 0.15, to: 0.85)
    XCTAssertTrue(address(app, is: "example.com"))
  }
}
