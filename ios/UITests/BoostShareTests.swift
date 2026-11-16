// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Shared Boosts (docs/design/boosts.md, "Sharing"): a share link opened in
/// a tab shows the preview instead of the page, and Add puts the Boost in
/// Settings → Boosts, marked Shared. The link needs no network: the tab
/// never loads it.
final class BoostShareTests: XCTestCase {
  /// {"v":1,"name":"UI Test Shared Boost","match":"example.org/ui-test-shared",
  /// "css":"p { color: red; }","js":"void 0;","js_world":"isolated"}
  private let link =
    "https://compassbrowser.org/boost#HYu7DsIwDEV_xfLc8ljTjY0ZmJGVWDQowVVsClLVf8d0vOecu-CM4djhiypjwNsZrqwGl5EaJziJqGGHlSyOrvlLdSq8k_bYv3Nvnva6pR5FVU8mWCBKkRbA8QCrm-dfzJITHIZt3j_SSnKYVQqZ39cf"
  private let boostName = "UI Test Shared Boost"

  override func setUp() {
    continueAfterFailure = false
  }

  private func rows(_ app: XCUIApplication) -> XCUIElementQuery {
    app.buttons.matching(NSPredicate(format: "identifier == 'BoostRow' AND label CONTAINS %@", boostName))
  }

  /// Some element's label has `text`.
  private func shows(_ app: XCUIApplication, _ text: String) -> Bool {
    app.descendants(matching: .any).matching(NSPredicate(format: "label CONTAINS %@", text)).firstMatch.exists
  }

  private func openBoostsSettings() -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["-CompassSettings", "YES"]
    app.launch()
    let boosts = app.buttons["SettingsBoosts"]
    XCTAssertTrue(boosts.waitForExistence(timeout: 30))
    boosts.tap()
    XCTAssertTrue(app.buttons["BoostsImport"].waitForExistence(timeout: 10))
    return app
  }

  private func deleteLeftovers(_ app: XCUIApplication) {
    while rows(app).firstMatch.exists {
      rows(app).firstMatch.swipeLeft()
      app.buttons["Delete"].firstMatch.tap()
      sleep(1)
    }
  }

  func testShareLinkInATabAddsTheBoost() {
    // Leftovers from an earlier run go first.
    var app = openBoostsSettings()
    deleteLeftovers(app)
    app.terminate()

    app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", link]
    app.launch()
    let add = app.buttons["BoostImportAdd"]
    XCTAssertTrue(add.waitForExistence(timeout: 30), "the preview")
    XCTAssertTrue(shows(app, boostName), "its name")
    XCTAssertTrue(shows(app, "example.org/ui-test-shared"), "its site")
    XCTAssertTrue(shows(app, "p { color: red; }"), "its CSS")
    XCTAssertTrue(shows(app, "void 0;"), "its JavaScript")
    XCTAssertTrue(app.descendants(matching: .any)["BoostImportJSWarning"].exists, "the warning")
    add.tap()
    XCTAssertTrue(add.waitForNonExistence(timeout: 10))
    app.terminate()

    app = openBoostsSettings()
    let row = rows(app).firstMatch
    XCTAssertTrue(row.waitForExistence(timeout: 10), "in the list")
    XCTAssertTrue(row.label.contains("Shared"), "marked Shared: \(row.label)")
    deleteLeftovers(app)
  }
}
