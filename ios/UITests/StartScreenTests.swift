// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Settings → On Launch: a tab is shown, the app goes to the background
/// (the sidebar and the last tab are saved) and is ended; the next launch
/// shows that tab (Last Tab) or the sidebar (Sidebar).
/// Open or closed is read from where the sidebar's "Pinned" header is.
final class StartScreenTests: XCTestCase {
  private var app: XCUIApplication!

  override func setUp() {
    continueAfterFailure = false
    app = XCUIApplication()
  }

  private func launch(_ arguments: [String]) {
    app.launchArguments = arguments
    app.launch()
    sleep(3)
    // The notification prompt (sync on) would cover the app.
    let allow = XCUIApplication(bundleIdentifier: "com.apple.springboard").buttons["Allow"]
    if allow.exists { allow.tap() }
  }

  /// Top of the sidebar's "Pinned" header as a fraction of the screen; past
  /// the bottom (or missing) when the sidebar is down.
  private var pinnedY: CGFloat {
    let header = app.staticTexts.matching(NSPredicate(format: "label ==[c] 'pinned'")).firstMatch
    return header.exists ? header.frame.minY / app.frame.height : 2
  }

  private var pageShown: Bool {
    app.webViews.staticTexts.containing(NSPredicate(format: "label CONTAINS 'documentation'")).firstMatch
      .waitForExistence(timeout: 10)
  }

  /// Shows example.org, then sends the app to the background and ends it.
  private func leaveOnPage(_ start: String) {
    launch(["-StartScreen", start, "-CompassOpenURL", "https://example.org/"])
    XCTAssertTrue(pageShown, "the page is up")
    XCUIDevice.shared.press(.home)
    sleep(2)
    app.terminate()
  }

  func testLastTab() {
    leaveOnPage("lastTab")
    launch(["-StartScreen", "lastTab"])
    XCTAssertTrue(pageShown, "the last tab is shown")
    XCTAssertGreaterThan(pinnedY, 0.95, "the sidebar is down")
  }

  func testSidebar() {
    leaveOnPage("sidebar")
    launch(["-StartScreen", "sidebar"])
    XCTAssertLessThan(pinnedY, 0.6, "the sidebar is up")
  }
}
