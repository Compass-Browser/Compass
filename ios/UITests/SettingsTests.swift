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
    pickEngine("Google")
    app.buttons["Done"].tap()
    sleep(1)
    app.buttons["BarNewTab"].tap()
    sleep(1)
    app.typeText("compass browser")
    let found = app.buttons.matching(NSPredicate(format: "label CONTAINS 'Search Google'")).firstMatch
      .waitForExistence(timeout: 5)
    // Back to the default for the other tests.
    app.terminate()
    app.launch()
    pickEngine("DuckDuckGo")
    XCTAssertTrue(found, "the command bar searches Google")
  }
}
