// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

final class SettingsTests: XCTestCase {
  private var app: XCUIApplication!

  private func pickEngine(_ name: String) {
    let row = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Search Engine'")).firstMatch
    XCTAssertTrue(row.waitForExistence(timeout: 20))
    row.tap()
    app.buttons[name].tap()
    app.navigationBars.buttons.element(boundBy: 0).tap()
  }

  /// The chosen search engine is the command bar's.
  func testSearchEngine() {
    app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", "https://example.org/", "-CompassSettings", "YES"]
    app.launch()
    pickEngine("Bing")
    app.buttons["Done"].tap()
    sleep(1)
    app.buttons["BarNewTab"].tap()
    sleep(1)
    app.typeText("compass browser")
    let found = app.buttons.matching(NSPredicate(format: "label CONTAINS 'Search Bing'")).firstMatch
      .waitForExistence(timeout: 5)
    // Back to the default for the other tests.
    app.terminate()
    app.launch()
    pickEngine("Google")
    XCTAssertTrue(found, "the command bar searches Bing")
  }

  /// Clear Browsing Data (all time) empties the command bar's history.
  func testClearBrowsingData() {
    app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", "https://example.org/"]
    app.launch()
    XCTAssertTrue(app.buttons["BarNewTab"].waitForExistence(timeout: 20))
    sleep(4)
    app.buttons["BarNewTab"].tap()
    XCTAssertTrue(app.staticTexts["RECENTLY VISITED"].waitForExistence(timeout: 5), "history to clear")
    app.buttons["Cancel"].tap()
    sleep(1)
    app.terminate()
    app.launchArguments = ["-CompassOpenURL", "https://example.org/", "-CompassSettings", "YES"]
    app.launch()
    let row = app.buttons["Clear Browsing Data"]
    XCTAssertTrue(row.waitForExistence(timeout: 20))
    row.tap()
    app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Time Range'")).firstMatch.tap()
    app.buttons["All Time"].tap()
    app.buttons["Clear Browsing Data"].firstMatch.tap()
    // The dialog's: the closed sidebar has a "Clear" too.
    app.sheets.buttons["Clear"].firstMatch.tap()
    sleep(2)
    app.buttons["Done"].tap()
    sleep(1)
    app.buttons["BarNewTab"].tap()
    sleep(1)
    XCTAssertFalse(app.staticTexts["RECENTLY VISITED"].exists, "history cleared")
  }

  /// Archive Inactive Tabs sets every space's, and Settings shows it.
  func testArchiveInactiveTabs() {
    app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", "https://example.org/", "-CompassSettings", "YES"]
    app.launch()
    let row = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Archive Inactive Tabs'")).firstMatch
    XCTAssertTrue(row.waitForExistence(timeout: 20))
    row.tap()
    app.buttons["After 7 Days"].tap()
    app.navigationBars.buttons.element(boundBy: 0).tap()
    XCTAssertTrue(row.label.contains("After 7 Days"), row.label)
    // Back to the default.
    row.tap()
    app.buttons["After 12 Hours"].tap()
  }
}
