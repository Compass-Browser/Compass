// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// The command bar's commands: one the text names leads and Return runs it;
/// part of a name lists it under "Commands", and Return searches.
final class CommandBarCommandsTests: XCTestCase {
  func testNamedCommandLeadsAndReturnRunsIt() {
    let app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", "https://example.org/"]
    app.launch()
    XCTAssertTrue(app.buttons["BarNewTab"].waitForExistence(timeout: 20))
    sleep(2)

    app.buttons["BarNewTab"].tap()
    sleep(1)
    app.typeText("sett")
    XCTAssertTrue(app.staticTexts["COMMANDS"].waitForExistence(timeout: 5), "listed under Commands")
    let command = app.buttons.matching(identifier: "CommandBarCommand").firstMatch
    XCTAssertTrue(command.exists)
    XCTAssertTrue(command.label.hasPrefix("Settings"), command.label)

    app.typeText("ings")
    XCTAssertTrue(app.staticTexts["COMMANDS"].waitForNonExistence(timeout: 5), "it leads, out of Commands")
    let search = app.buttons.matching(NSPredicate(format: "label CONTAINS 'Search '")).firstMatch
    XCTAssertTrue(command.exists && search.exists)
    XCTAssertLessThan(command.frame.minY, search.frame.minY, "above the search row")

    app.typeText("\n")
    XCTAssertTrue(app.buttons["Clear Browsing Data"].waitForExistence(timeout: 5), "Settings opened")
  }
}
