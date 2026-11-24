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

  /// Swiping follows the tab picker's cards: right goes to the tab shown
  /// before, left comes back (the order holds through a run of swipes); a
  /// short swipe springs back to the same one, and past the most recent tab
  /// there's nothing.
  func testSwipeChangesTab() {
    let app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURLs", "https://example.org/ https://example.com/"]
    app.launch()
    XCTAssertTrue(address(app, is: "example.com"))
    swipeBar(app, from: 0.5, to: 0.42)
    XCTAssertTrue(address(app, is: "example.com"))
    swipeBar(app, from: 0.85, to: 0.15)
    XCTAssertTrue(address(app, is: "example.com"))
    swipeBar(app, from: 0.15, to: 0.85)
    XCTAssertTrue(address(app, is: "example.org"))
    swipeBar(app, from: 0.85, to: 0.15)
    XCTAssertTrue(address(app, is: "example.com"))
  }

  /// Once the swiping has stopped for a moment, the tab swiped to is the
  /// most recent: right goes back to the one before it.
  func testSwipeOrderSettles() {
    let app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURLs", "https://example.org/ https://example.com/"]
    app.launch()
    XCTAssertTrue(address(app, is: "example.com"))
    swipeBar(app, from: 0.15, to: 0.85)
    XCTAssertTrue(address(app, is: "example.org"))
    sleep(3)
    swipeBar(app, from: 0.15, to: 0.85)
    XCTAssertTrue(address(app, is: "example.com"))
  }
}
