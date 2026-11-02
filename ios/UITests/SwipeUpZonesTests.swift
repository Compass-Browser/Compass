// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Where a swipe up on the bar starts decides what it opens: on the address
/// pill (all of it) the sidebar, left of it the tab picker, right of it the
/// command bar for a new tab.
final class SwipeUpZonesTests: XCTestCase {
  private var app: XCUIApplication!

  override func setUp() {
    continueAfterFailure = false
    app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURLs", "https://example.org/ https://example.com/"]
    app.launch()
    XCTAssertTrue(app.buttons["Address"].waitForExistence(timeout: 20))
    sleep(3)
  }

  /// A fast swipe up from `x` (points) at the pill's height.
  private func swipeUp(fromX x: CGFloat) {
    let pill = app.buttons["Address"].frame
    let start = app.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: x, dy: pill.midY))
    let end = app.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: x, dy: app.frame.height * 0.15))
    start.press(forDuration: 0.05, thenDragTo: end, withVelocity: .fast, thenHoldForDuration: 0)
    sleep(2)
  }

  /// Near the pill's left end, inside the old left third: the sidebar.
  func testPillLeftEndOpensSidebar() {
    let pill = app.buttons["Address"].frame
    XCTAssertLessThan(pill.minX + 8, app.frame.width / 3, "the pill reaches into the left third")
    swipeUp(fromX: pill.minX + 8)
    XCTAssertEqual(app.buttons.matching(identifier: "PickerCard").count, 0, "no tab picker")
    XCTAssertFalse(app.textFields["Search or enter address"].exists, "no command bar")
    XCTAssertTrue(app.buttons["Settings"].firstMatch.isHittable, "sidebar open")
  }

  /// Near the pill's right end, inside the old right third: the sidebar.
  func testPillRightEndOpensSidebar() {
    let pill = app.buttons["Address"].frame
    XCTAssertGreaterThan(pill.maxX - 8, app.frame.width * 2 / 3, "the pill reaches into the right third")
    swipeUp(fromX: pill.maxX - 8)
    XCTAssertFalse(app.textFields["Search or enter address"].exists, "no command bar")
    XCTAssertEqual(app.buttons.matching(identifier: "PickerCard").count, 0, "no tab picker")
    XCTAssertTrue(app.buttons["Settings"].firstMatch.isHittable, "sidebar open")
  }

  /// On the back button: the tab picker.
  func testBackButtonOpensPicker() {
    swipeUp(fromX: app.buttons["Back"].frame.midX)
    XCTAssertGreaterThan(app.buttons.matching(identifier: "PickerCard").count, 0, "tab picker open")
  }

  /// On the new tab button: the command bar.
  func testNewTabButtonOpensCommandBar() {
    swipeUp(fromX: app.buttons["BarNewTab"].frame.midX)
    XCTAssertTrue(app.textFields["Search or enter address"].waitForExistence(timeout: 3), "command bar open")
  }
}
