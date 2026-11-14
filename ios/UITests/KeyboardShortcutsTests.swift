// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Hardware-keyboard shortcuts (BrowserCommands), typed as real key events.
/// Run on an iPad simulator for the sidebar column (⌘S).
final class KeyboardShortcutsTests: XCTestCase {
  private var app: XCUIApplication!

  override func setUp() {
    continueAfterFailure = false
    app = XCUIApplication()
    app.launchArguments = ["-CompassFreshStore", "YES", "-CompassOpenURLs", "https://example.org/ https://example.com/"]
    app.launch()
    XCTAssertTrue(address(is: "example.com"))
  }

  private func address(is host: String) -> Bool {
    for _ in 0..<50 {
      if app.buttons["Address"].value as? String == host { return true }
      usleep(100_000)
    }
    return false
  }

  private func waitFor(_ condition: () -> Bool) -> Bool {
    for _ in 0..<30 {
      if condition() { return true }
      usleep(100_000)
    }
    return false
  }

  /// ⌘T opens the command bar for a new tab.
  func testCommandTOpensCommandBar() {
    app.typeKey("t", modifierFlags: .command)
    XCTAssertTrue(app.textFields.firstMatch.waitForExistence(timeout: 3))
  }

  /// ⌘S hides the iPad's sidebar column, and shows it again.
  func testCommandSTogglesSidebarColumn() throws {
    guard UIDevice.current.userInterfaceIdiom == .pad else { throw XCTSkip("iPad only") }
    let toggle = app.buttons["BarSidebar"]
    XCTAssertTrue(toggle.waitForExistence(timeout: 5))
    XCTAssertEqual(toggle.label, "Hide sidebar")
    app.typeKey("s", modifierFlags: .command)
    XCTAssertTrue(waitFor { toggle.label == "Show sidebar" })
    app.typeKey("s", modifierFlags: .command)
    XCTAssertTrue(waitFor { toggle.label == "Hide sidebar" })
  }

  /// Puts the focus in the page, which then gets Tab (the reason ⌃Tab once
  /// worked a single time).
  private func focusPage() {
    let page = app.webViews.firstMatch
    if page.waitForExistence(timeout: 5) { page.tap() }
  }

  /// ⌃Tab steps to the next entry every time, also with the page focused,
  /// and ⌃⇧Tab back (BrowserKeys).
  func testControlTabStepsBetweenEntries() {
    focusPage()
    app.typeKey("\t", modifierFlags: .control)
    XCTAssertTrue(address(is: "example.org"))
    focusPage()
    app.typeKey("\t", modifierFlags: .control)
    XCTAssertTrue(address(is: "example.com"))
    app.typeKey("\t", modifierFlags: .control)
    XCTAssertTrue(address(is: "example.org"))
    focusPage()
    app.typeKey("\t", modifierFlags: [.control, .shift])
    XCTAssertTrue(address(is: "example.com"))
  }

  /// ⌘W closes the shown Today tab (to the archive), not the window.
  func testCommandWClosesTabNotWindow() {
    focusPage()
    let closeButtons = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Close '"))
    let pad = UIDevice.current.userInterfaceIdiom == .pad
    if pad { XCTAssertTrue(waitFor { closeButtons.count == 2 }) }
    app.typeKey("w", modifierFlags: .command)
    XCTAssertTrue(waitFor { app.buttons["Address"].value as? String != "example.com" })
    if pad { XCTAssertTrue(waitFor { closeButtons.count == 1 }) }
    // The window is still there.
    sleep(1)
    XCTAssertEqual(app.state, .runningForeground)
    XCTAssertTrue(app.buttons["Address"].exists || app.buttons["BarSidebar"].exists)
  }

  /// The sidebar column's edge drags wider (iPad).
  func testSidebarColumnResizes() throws {
    guard UIDevice.current.userInterfaceIdiom == .pad else { throw XCTSkip("iPad only") }
    let edge = app.otherElements["SidebarResize"]
    let toggle = app.buttons["BarSidebar"]
    XCTAssertTrue(edge.waitForExistence(timeout: 5))
    XCTAssertTrue(toggle.waitForExistence(timeout: 5))
    // From the default width.
    edge.doubleTap()
    sleep(1)
    let before = toggle.frame.minX
    let start = edge.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5))
    start.press(forDuration: 0.1, thenDragTo: start.withOffset(CGVector(dx: 100, dy: 0)))
    XCTAssertTrue(waitFor { toggle.frame.minX > before + 50 }, "\(before) → \(toggle.frame.minX)")
    edge.doubleTap()
    XCTAssertTrue(waitFor { abs(toggle.frame.minX - before) < 2 })
  }
}

/// A split's panes side by side (iPad): dragging one pane's title strip
/// onto the other's right half moves it there.
final class SplitPaneDragTests: XCTestCase {
  func testDraggingAPaneStripReorders() throws {
    guard UIDevice.current.userInterfaceIdiom == .pad else { throw XCTSkip("iPad only") }
    let app = XCUIApplication()
    app.launchArguments = [
      "-CompassFreshStore", "YES", "-CompassOpenURLs", "https://example.org/ https://example.com/", "-CompassSplit", "YES",
    ]
    app.launch()
    let strips = app.buttons.matching(identifier: "PaneStrip")
    for _ in 0..<80 where strips.count < 2 { usleep(100_000) }
    XCTAssertEqual(strips.count, 2)
    // The panes' order, left to right, by their icons' sites in the
    // address after a tap on each.
    func order() -> [String] {
      (0..<strips.count).map { strips.element(boundBy: $0) }.sorted { $0.frame.minX < $1.frame.minX }.map { strip in
        strip.tap()
        usleep(400_000)
        return app.buttons["Address"].value as? String ?? ""
      }
    }
    let first = order()
    XCTAssertEqual(Set(first), ["example.org", "example.com"])
    let sorted = (0..<2).map { strips.element(boundBy: $0) }.sorted { $0.frame.minX < $1.frame.minX }
    let left = sorted[0], right = sorted[1]
    left.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5))
      .press(forDuration: 1.2, thenDragTo: right.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.5)),
             withVelocity: .slow, thenHoldForDuration: 0.6)
    var now: [String] = []
    for _ in 0..<20 {
      now = order()
      if now == first.reversed() { break }
      usleep(300_000)
    }
    XCTAssertEqual(now, first.reversed())
  }
}
