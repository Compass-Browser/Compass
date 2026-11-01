// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Scrolling a page down collapses the bottom bar; scrolling up, or a tap
/// on the small pill, brings it back.
final class CollapseBarTests: XCTestCase {
  func testScrollCollapsesAndRestores() {
    let app = XCUIApplication()
    // A long page.
    app.launchArguments = ["-CompassOpenURL", "https://en.wikipedia.org/wiki/Compass"]
    app.launch()
    XCTAssertTrue(app.buttons["Menu"].waitForExistence(timeout: 20))
    sleep(6)
    let page = app.webViews.firstMatch
    page.swipeUp()
    XCTAssertTrue(app.buttons["Show toolbar"].waitForExistence(timeout: 3), "collapsed")
    XCTAssertFalse(app.buttons["Menu"].exists)
    page.swipeDown()
    XCTAssertTrue(app.buttons["Menu"].waitForExistence(timeout: 3), "back on scroll up")
    page.swipeUp()
    XCTAssertTrue(app.buttons["Show toolbar"].waitForExistence(timeout: 3))
    app.buttons["Show toolbar"].tap()
    XCTAssertTrue(app.buttons["Menu"].waitForExistence(timeout: 3), "back on tap")
  }
}
