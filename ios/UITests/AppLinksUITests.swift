// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Links to apps (docs/design/ios.md, "Links to apps"), on a page with a
/// link to a scheme no app has. Serve ios/UITests/Fixtures over HTTP first:
///   python3 -m http.server -d ios/UITests/Fixtures 8768
/// (COMPASS_APP_LINKS_BASE overrides http://127.0.0.1:8768/).
final class AppLinksUITests: XCTestCase {
  private var base: String {
    ProcessInfo.processInfo.environment["COMPASS_APP_LINKS_BASE"] ?? "http://127.0.0.1:8768/"
  }

  private func launch(_ page: String) -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", base + page]
    app.launch()
    XCTAssertTrue(app.buttons["Menu"].waitForExistence(timeout: 60))
    return app
  }

  /// A tapped link no app can open: the toast says so, the page stays (no
  /// error page).
  func testTappedLinkToMissingApp() {
    let app = launch("app_links.html")
    let link = app.webViews.links["Open the missing app"]
    XCTAssertTrue(link.waitForExistence(timeout: 30), "the page loaded")
    link.tap()
    XCTAssertTrue(app.staticTexts["No app can open this link"].waitForExistence(timeout: 10), "the toast")
    XCTAssertFalse(app.alerts.firstMatch.exists, "no prompt after a tap")
    XCTAssertTrue(app.webViews.links["Open the missing app"].exists, "still on the page")
  }

  /// A page going to an app on its own: asked first; Cancel stays.
  func testRedirectAsksFirst() {
    let app = launch("app_links.html?redirect")
    let alert = app.alerts["Open in app?"]
    XCTAssertTrue(alert.waitForExistence(timeout: 30), "the prompt")
    alert.buttons["Cancel"].tap()
    XCTAssertTrue(alert.waitForNonExistence(timeout: 10))
    XCTAssertFalse(app.staticTexts["No app can open this link"].exists)
    XCTAssertTrue(app.webViews.links["Open the missing app"].exists, "still on the page")
  }
}
