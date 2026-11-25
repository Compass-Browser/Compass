// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// The website's iPhone screenshots (website/img/), from the demo sidebar
/// (-CompassDemo) in the simulator. Not part of the regular suite: it skips
/// unless WEBSITE_SHOTS is set. tools/website/capture_ios.sh runs it, sets
/// the status bar, and exports the attachments as PNGs.
///
/// Each shot is an attachment named "website-<name>".
final class WebsiteScreenshotsTests: XCTestCase {
  private var app: XCUIApplication!

  private func shot(_ name: String) {
    let attachment = XCTAttachment(screenshot: XCUIScreen.main.screenshot())
    attachment.name = "website-\(name)"
    attachment.lifetime = .keepAlways
    add(attachment)
  }

  func testWebsiteScreenshots() throws {
    try XCTSkipUnless(ProcessInfo.processInfo.environment["WEBSITE_SHOTS"] != nil,
                      "website screenshots: set WEBSITE_SHOTS")
    continueAfterFailure = true
    app = XCUIApplication()
    // The demo sidebar, never the Keychain's sync chain, and a page open.
    app.launchArguments = [
      "-CompassDemo", "YES", "-CompassNoSync", "YES",
      "-CompassOpenURL", "https://en.wikipedia.org/wiki/Lisbon",
    ]
    app.launch()
    sleep(8)
    // Wikipedia's banner for its own app, when it shows one.
    let close = app.webViews.descendants(matching: .any)
      .matching(NSPredicate(format: "label CONTAINS[c] 'close' AND (elementType == %d OR elementType == %d)",
                            XCUIElement.ElementType.button.rawValue, XCUIElement.ElementType.link.rawValue))
      .firstMatch
    if close.exists {
      close.tap()
      sleep(2)
    }
    shot("page")

    // The sidebar: a swipe up on the bottom bar.
    let bar = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.91))
    bar.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.1)),
              withVelocity: .fast, thenHoldForDuration: 0)
    sleep(2)
    shot("sidebar")
    let grabber = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.085))
    grabber.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.98)))
    sleep(2)

    // The command bar, with suggestions for a few letters.
    let newTab = app.buttons["BarNewTab"]
    if newTab.waitForExistence(timeout: 5) {
      newTab.tap()
      sleep(1)
      app.typeText("sintra")
      sleep(3)
      shot("commandbar")
    } else {
      XCTFail("missing the bar's new tab button")
    }
  }
}
