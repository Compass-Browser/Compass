// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// The sidebar panel with real touches: open with the button, close with
/// the grabber or a pull on the list, scroll the list without closing.
/// Open or closed is read from where the space's name is on screen.
final class SidebarPanelTests: XCTestCase {
  private var app: XCUIApplication!

  override func setUp() {
    continueAfterFailure = false
    app = XCUIApplication()
    // A page shown, so the sidebar starts closed.
    app.launchArguments = ["-CompassOpenURL", "https://example.org/"]
    app.launch()
    sleep(3)
  }

  /// The top of the shown space's header, as a fraction of the screen.
  private var headerY: CGFloat {
    let header = app.staticTexts.matching(identifier: "Personal").firstMatch
    return header.frame.minY / app.frame.height
  }

  private func openWithButton() {
    app.buttons["Sidebar"].tap()
    sleep(1)
    XCTAssertLessThan(headerY, 0.3, "sidebar open at full height")
  }

  func testButtonOpens() {
    XCTAssertGreaterThan(headerY, 0.95, "starts closed")
    openWithButton()
  }

  func testGrabberDragCloses() {
    openWithButton()
    let grabber = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.085))
    grabber.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.98)))
    sleep(1)
    XCTAssertGreaterThan(headerY, 0.95, "closed")
  }

  func testListPullCloses() {
    openWithButton()
    let list = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.45))
    list.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.9)))
    sleep(1)
    XCTAssertGreaterThan(headerY, 0.95, "closed")
  }

  func testListScrollStaysOpen() {
    openWithButton()
    let list = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.7))
    list.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.4)))
    sleep(1)
    // Scrolled: the header moved up, but the panel is still up.
    XCTAssertLessThan(headerY, 0.3, "still open")
    let grabber = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.085))
    _ = grabber
  }
}
