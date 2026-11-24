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

  /// From an incognito tab the picker shows incognito's tabs only.
  func testIncognitoPickerShowsIncognitoTabs() {
    let app = XCUIApplication()
    app.launchArguments = ["-CompassIncognitoURL", "https://example.net/", "-CompassPicker", "YES"]
    app.launch()
    XCTAssertTrue(app.buttons["PickerCard"].firstMatch.waitForExistence(timeout: 20))
    XCTAssertEqual(app.buttons.matching(identifier: "PickerCard").count, 1)
    XCTAssertTrue(app.buttons["PickerIncognito"].isSelected)
    XCTAssertEqual(app.buttons["PickerCard"].firstMatch.value as? String, "Incognito")
  }

  /// From a regular tab it shows the regular ones; the toggle switches to
  /// incognito's (none here).
  func testToggleSwitchesToIncognito() {
    let app = launchWithPicker()
    XCTAssertTrue(app.buttons["PickerRegular"].isSelected)
    XCTAssertEqual(app.buttons.matching(identifier: "PickerCard").count, 2)
    app.buttons["PickerIncognito"].tap()
    XCTAssertTrue(app.staticTexts["No incognito tabs"].waitForExistence(timeout: 2))
    XCTAssertEqual(app.buttons.matching(identifier: "PickerCard").count, 0)
  }

  /// The + held offers the bar's + menu; New Incognito Tab closes the
  /// picker onto the incognito space.
  func testPlusMenuNewIncognitoTab() {
    let app = launchWithPicker()
    app.buttons["PickerNewTab"].press(forDuration: 1.0)
    let item = app.buttons["New Incognito Tab"]
    XCTAssertTrue(item.waitForExistence(timeout: 3))
    XCTAssertTrue(app.buttons["New Easel"].exists)
    item.tap()
    XCTAssertTrue(pickerGone(app))
    let note = app.staticTexts.containing(NSPredicate(format: "label BEGINSWITH 'Incognito tabs aren'")).firstMatch
    XCTAssertTrue(note.waitForExistence(timeout: 5))
  }
}
