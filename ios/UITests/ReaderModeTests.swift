// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// Reader mode (docs/design/reader-mode.md, "iOS") on the shared package's
/// own test pages. Serve common/reader over HTTP first:
///   python3 -m http.server -d common/reader 8767
/// (COMPASS_READER_BASE overrides http://127.0.0.1:8767/).
final class ReaderModeTests: XCTestCase {
  private var base: String {
    ProcessInfo.processInfo.environment["COMPASS_READER_BASE"] ?? "http://127.0.0.1:8767/"
  }

  private func launch(_ page: String) -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", base + "test/fixtures/" + page]
    app.launch()
    XCTAssertTrue(app.buttons["Menu"].waitForExistence(timeout: 60))
    return app
  }

  /// An article: the bar's book shows; it puts the article over the page
  /// (the shared reader page, its toolbar and text), and the reader's ×
  /// goes back to the page.
  func testArticle() {
    let app = launch("article.html")
    let button = app.buttons["BarReader"]
    XCTAssertTrue(button.waitForExistence(timeout: 60), "the Reader button shows")
    XCTAssertEqual(button.label, "Show Reader")
    button.tap()
    let reader = app.webViews["ReaderPage"]
    XCTAssertTrue(reader.waitForExistence(timeout: 30), "the reader is up")
    let title = reader.staticTexts.containing(NSPredicate(format: "label CONTAINS 'Lighthouse Keepers of Capo Rosso'"))
    XCTAssertTrue(title.firstMatch.waitForExistence(timeout: 30), "the article's title")
    XCTAssertTrue(
      reader.staticTexts.containing(NSPredicate(format: "label CONTAINS 'Rosa Bellini'")).firstMatch.exists,
      "the article's text")
    XCTAssertFalse(reader.staticTexts.containing(NSPredicate(format: "label CONTAINS 'Sponsored'")).firstMatch.exists,
                   "not the page's ads")
    XCTAssertEqual(app.buttons["BarReader"].label, "Hide Reader")
    // The shared page's own toolbar: Aa opens its settings.
    reader.buttons["Text and colors"].tap()
    // The panel's choices are radios (role="radio").
    let sepiaChoice = choice("Sepia", in: reader)
    XCTAssertTrue(sepiaChoice.waitForExistence(timeout: 10), "the settings panel: \(reader.debugDescription)")
    sepiaChoice.tap()
    reader.buttons["Leave Reader"].tap()
    XCTAssertTrue(reader.waitForNonExistence(timeout: 15), "back to the page")
    XCTAssertEqual(app.buttons["BarReader"].label, "Show Reader")
    // The theme was kept: the next reader is sepia too (checked by the
    // swatch being on).
    app.buttons["BarReader"].tap()
    let again = app.webViews["ReaderPage"]
    XCTAssertTrue(again.waitForExistence(timeout: 30))
    XCTAssertTrue(again.buttons["Text and colors"].waitForExistence(timeout: 30), "the reader page loaded")
    again.buttons["Text and colors"].tap()
    let sepia = choice("Sepia", in: again)
    XCTAssertTrue(sepia.waitForExistence(timeout: 10))
    let chosen = sepia.isSelected || "\(sepia.value ?? "")" == "1"
    XCTAssertTrue(chosen, "Sepia is still chosen: \(String(describing: sepia.value))")
    XCTAssertFalse(choice("Auto", in: again).isSelected)
    choice("Auto", in: again).tap()
  }

  /// A choice in the reader's settings panel (role="radio"), whatever
  /// WebKit exposes it as.
  private func choice(_ label: String, in reader: XCUIElement) -> XCUIElement {
    reader.descendants(matching: .any).matching(NSPredicate(format: "label == %@", label)).firstMatch
  }

  /// A sign-in page isn't an article: no Reader button.
  func testNotAnArticle() {
    let app = launch("not_article.html")
    sleep(5)
    let button = app.buttons["BarReader"]
    XCTAssertFalse(button.exists)
  }
}
