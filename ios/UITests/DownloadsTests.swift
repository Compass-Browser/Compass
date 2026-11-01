// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// A page sent as an attachment is downloaded and listed in the menu's
/// Downloads.
final class DownloadsTests: XCTestCase {
  func testAttachmentIsDownloaded() {
    let name = "compass-ui-\(Int.random(in: 1000...9999)).txt"
    let app = XCUIApplication()
    app.launchArguments = [
      "-CompassOpenURL",
      "https://httpbin.org/response-headers?Content-Disposition=attachment%3B%20filename%3D\(name)",
    ]
    app.launch()
    XCTAssertTrue(app.buttons["Menu"].waitForExistence(timeout: 20))
    sleep(4)
    app.buttons["Menu"].tap()
    XCTAssertTrue(app.buttons["Downloads"].waitForExistence(timeout: 5))
    app.buttons["Downloads"].tap()
    XCTAssertTrue(app.staticTexts[name].waitForExistence(timeout: 5), "listed")
  }
}
