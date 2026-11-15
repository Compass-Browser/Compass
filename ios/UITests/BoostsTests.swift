// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Boosts (docs/design/boosts.md): a CSS Boost made from the page menu
/// applies to the page once it reloads, and goes again when deleted.
final class BoostsTests: XCTestCase {
  private var app: XCUIApplication!

  override func setUp() {
    continueAfterFailure = false
    app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", "https://example.org/"]
    app.launch()
    sleep(4)
  }

  /// The page's paragraph (not its title, which the tab shows too).
  private var paragraph: XCUIElement {
    app.staticTexts.containing(NSPredicate(format: "label CONTAINS %@", "This domain is for use")).firstMatch
  }

  private func openSiteBoosts() {
    app.buttons["Menu"].tap()
    sleep(1)
    app.buttons["MenuBoosts"].tap()
    sleep(1)
  }

  /// Done in the site's Boosts, then away from the page menu under it.
  private func closeSiteBoosts() {
    app.buttons["Done"].tap()
    sleep(1)
    app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.08)).tap()
    sleep(3)
  }

  /// Deletes the site's first Boost (swipe, Delete).
  private func deleteFirstBoost() {
    app.buttons["BoostRow"].firstMatch.swipeLeft()
    app.buttons["Delete"].firstMatch.tap()
    sleep(1)
  }

  func testCSSBoostApplies() {
    // Boosts left from an earlier run go first.
    openSiteBoosts()
    while app.buttons["BoostRow"].exists { deleteFirstBoost() }
    closeSiteBoosts()
    XCTAssertTrue(paragraph.waitForExistence(timeout: 10))

    openSiteBoosts()
    app.buttons["BoostsNewForSite"].tap()
    let css = app.textViews["BoostCSS"]
    XCTAssertTrue(css.waitForExistence(timeout: 3))
    css.tap()
    css.typeText("p { display: none !important; }")
    app.buttons["BoostSave"].tap()
    sleep(1)
    closeSiteBoosts()
    // The page reloaded with the Boost: its paragraph is hidden.
    XCTAssertFalse(paragraph.exists)

    // Delete it: the paragraph comes back.
    openSiteBoosts()
    deleteFirstBoost()
    closeSiteBoosts()
    XCTAssertTrue(paragraph.waitForExistence(timeout: 10))
  }
}
