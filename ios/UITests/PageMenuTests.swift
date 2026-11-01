// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// The bottom bar's menu, with real touches.
final class PageMenuTests: XCTestCase {
  private var app: XCUIApplication!

  override func setUp() {
    continueAfterFailure = false
    app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", "https://example.org/"]
    app.launch()
    sleep(4)
  }

  func testCopyLink() {
    app.buttons["Menu"].tap()
    sleep(1)
    // The runner can't read the app's pasteboard: the button says it copied.
    app.buttons["Copy Link"].tap()
    XCTAssertEqual(app.buttons["Copy Link"].value as? String, "Copied")
  }

  func testFindFromMenu() {
    app.buttons["Menu"].tap()
    sleep(1)
    app.buttons["Find"].tap()
    sleep(2)
    // The find navigator's search field.
    XCTAssertTrue(app.searchFields.firstMatch.waitForExistence(timeout: 3) || app.textFields.firstMatch.exists)
  }

  func testPinThenUnpin() {
    app.buttons["Menu"].tap()
    sleep(1)
    app.buttons["Pin"].tap()
    sleep(1)
    XCTAssertTrue(app.buttons["Unpin"].exists, "pinned")
    app.buttons["Unpin"].tap()
    sleep(1)
    XCTAssertTrue(app.buttons["Pin"].exists, "unpinned")
  }

  func testFindFromCommandBar() {
    app.buttons["Address"].tap()
    sleep(1)
    app.typeText("domain")
    sleep(1)
    app.buttons.matching(NSPredicate(format: "label BEGINSWITH %@", "Find “domain”")).firstMatch.tap()
    sleep(2)
    XCTAssertTrue(app.searchFields.firstMatch.waitForExistence(timeout: 3) || app.textFields.firstMatch.exists)
  }
}
