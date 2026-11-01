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

  /// A swipe up on the bottom bar, let go near the top.
  private func openSidebar() {
    let bar = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.91))
    bar.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.1)),
              withVelocity: .fast, thenHoldForDuration: 0)
    sleep(1)
    XCTAssertLessThan(headerY, 0.3, "sidebar open at full height")
  }

  func testSwipeOpens() {
    XCTAssertGreaterThan(headerY, 0.95, "starts closed")
    openSidebar()
  }

  func testGrabberDragCloses() {
    openSidebar()
    let grabber = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.085))
    grabber.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.98)))
    sleep(1)
    XCTAssertGreaterThan(headerY, 0.95, "closed")
  }

  func testListPullCloses() {
    openSidebar()
    let list = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.45))
    list.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.9)))
    sleep(1)
    XCTAssertGreaterThan(headerY, 0.95, "closed")
  }

  func testListScrollStaysOpen() {
    openSidebar()
    let list = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.7))
    list.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.4)))
    sleep(1)
    // Scrolled: the header moved up, but the panel is still up.
    XCTAssertLessThan(headerY, 0.3, "still open")
  }

  /// At half height a drag on the list moves the panel, as a sheet does.
  func testHalfHeightListDragExpands() {
    openSidebar()
    let grabber = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.085))
    grabber.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)))
    sleep(1)
    XCTAssertGreaterThan(headerY, 0.45, "at half height")
    XCTAssertLessThan(headerY, 0.8, "at half height")
    let list = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.8))
    list.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.3)))
    sleep(1)
    XCTAssertLessThan(headerY, 0.3, "back at full height")
  }

  /// At half height a drag down on the list closes it.
  func testHalfHeightListDragCloses() {
    openSidebar()
    let grabber = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.085))
    grabber.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5)))
    sleep(1)
    let list = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.7))
    list.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.99)))
    sleep(1)
    XCTAssertGreaterThan(headerY, 0.95, "closed")
  }
}
