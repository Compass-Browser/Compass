// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// The tab picker, opened at launch over two loaded tabs.
final class TabPickerTests: XCTestCase {
  private func launchWithPicker() -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = [
      "-CompassOpenURLs", "https://example.org/ https://example.com/", "-CompassPicker", "YES",
    ]
    app.launch()
    XCTAssertTrue(app.buttons["PickerCard"].firstMatch.waitForExistence(timeout: 20))
    return app
  }

  /// Polls until no card is left (the picker closed), up to 3 s.
  private func pickerGone(_ app: XCUIApplication) -> Bool {
    for _ in 0..<30 {
      if app.buttons.matching(identifier: "PickerCard").count == 0 { return true }
      usleep(100_000)
    }
    return false
  }

  /// A tap on the stacked card shows its tab and closes the picker.
  func testTapPicksTab() {
    let app = launchWithPicker()
    XCTAssertEqual(app.buttons.matching(identifier: "PickerCard").count, 2)
    app.coordinate(withNormalizedOffset: CGVector(dx: 0.08, dy: 0.5)).tap()
    XCTAssertTrue(pickerGone(app))
    XCTAssertEqual(app.buttons["Address"].value as? String, "example.org")
  }

  /// Flicking every card away closes the picker by itself.
  func testLastCardClosedClosesPicker() {
    let app = launchWithPicker()
    for _ in 0..<2 {
      let center = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.55))
      center.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.05)),
        withVelocity: .fast, thenHoldForDuration: 0)
      sleep(1)
    }
    XCTAssertTrue(pickerGone(app))
    XCTAssertTrue(app.buttons["Address"].waitForExistence(timeout: 2))
  }
}
