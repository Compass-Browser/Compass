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
}
