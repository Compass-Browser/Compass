// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Auto picture-in-picture with real touches. Needs a page with a video at
/// COMPASS_PIP_PAGE (default http://127.0.0.1:8765/v.html, which shows
/// `paused=… mode=…` under the video, and `pip-seen` once it was in
/// picture-in-picture; its play button is near the top). The window itself is outside the
/// app; the video's presentation mode, as the page prints it, tells.
/// Device only: the simulator has no picture-in-picture (even Safari's
/// `document.pictureInPictureEnabled` is false there).
final class AutoPiPTests: XCTestCase {
  private var app: XCUIApplication!
  private var page: String { ProcessInfo.processInfo.environment["COMPASS_PIP_PAGE"] ?? "http://127.0.0.1:8765/v.html" }

  override func setUp() {
    continueAfterFailure = false
    app = XCUIApplication()
    app.launchArguments = [
      "-CompassOpenURLs", "https://example.org/ \(page)?\(Int(Date().timeIntervalSince1970))",
    ]
    app.launch()
    XCTAssertTrue(status("mode=inline", timeout: 30), "the page is up")
    // A real tap on the video's play button (the page's own gesture).
    app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.2)).tap()
    XCTAssertTrue(app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'paused=false'")).firstMatch
      .waitForExistence(timeout: 30), "the video plays")
  }

  private func status(_ text: String, timeout: TimeInterval = 8) -> Bool {
    app.staticTexts.containing(NSPredicate(format: "label CONTAINS %@", text)).firstMatch.waitForExistence(timeout: timeout)
  }

  private func swipeBar(from: CGFloat, to: CGFloat) {
    let start = app.coordinate(withNormalizedOffset: CGVector(dx: from, dy: 0.91))
    let end = app.coordinate(withNormalizedOffset: CGVector(dx: to, dy: 0.91))
    start.press(forDuration: 0.05, thenDragTo: end, withVelocity: .slow, thenHoldForDuration: 0)
  }

  /// Leaving the tab moves the video to picture-in-picture; coming back
  /// puts it back in the page, still playing.
  func testTabSwitch() {
    swipeBar(from: 0.85, to: 0.15)
    sleep(4)
    swipeBar(from: 0.15, to: 0.85)
    XCTAssertTrue(status("mode=inline"), "back inline")
    XCTAssertTrue(status("paused=false"), "still playing")
    // The page logged the trip: it was in picture-in-picture meanwhile.
    let line = app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'paused='")).firstMatch.label
    XCTAssertTrue(line.contains("pip-seen"), "was in picture-in-picture: \(line)")
  }

  /// Home moves it to picture-in-picture; back in the app, it returns.
  func testLeaveApp() {
    XCUIDevice.shared.press(.home)
    sleep(4)
    app.activate()
    XCTAssertTrue(status("mode=inline"), "back inline")
    XCTAssertTrue(app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'pip-seen'")).firstMatch.exists,
                  "was in picture-in-picture")
  }
}
