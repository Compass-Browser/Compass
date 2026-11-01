// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// The theme editor with real touches: a gradient preset, a drag on the pad,
/// one color more and less, the mode and the sliders; the sidebar keeps the
/// theme after Done. Screenshots go into the test's attachments.
final class ThemeEditorTests: XCTestCase {
  private var app: XCUIApplication!

  override func setUp() {
    continueAfterFailure = false
    app = XCUIApplication()
    app.launchArguments = ["-CompassOpenURL", "https://example.org/"]
    app.launch()
    sleep(3)
  }

  private func openSidebar() {
    let bar = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.91))
    bar.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.1)),
              withVelocity: .fast, thenHoldForDuration: 0)
    sleep(1)
  }

  private func snapshot(_ name: String) {
    let attachment = XCTAttachment(screenshot: app.screenshot())
    attachment.name = name
    attachment.lifetime = .keepAlways
    add(attachment)
  }

  func testEditsTheSpaceTheme() {
    openSidebar()
    app.buttons["Space menu"].firstMatch.tap()
    sleep(1)
    app.buttons["Edit Theme"].tap()
    XCTAssertTrue(app.otherElements["ThemePad"].waitForExistence(timeout: 3))
    snapshot("editor")

    // A whole gradient: three dots and their path.
    app.buttons["Ember"].tap()
    sleep(1)
    XCTAssertTrue(app.otherElements["Gradient color 2"].exists, "three colors")
    snapshot("ember")

    // − then +.
    app.buttons["Fewer colors"].tap()
    XCTAssertFalse(app.otherElements["Gradient color 2"].exists, "two colors")
    app.buttons["More colors"].tap()
    XCTAssertTrue(app.otherElements["Gradient color 2"].exists, "three again")

    // Dragging the big dot takes the others along.
    let pad = app.otherElements["ThemePad"]
    let color = app.otherElements["Color"]
    let before = app.otherElements["Gradient color 1"].frame.midX
    color.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5))
      .press(forDuration: 0.1, thenDragTo: pad.coordinate(withNormalizedOffset: CGVector(dx: 0.15, dy: 0.5)))
    sleep(1)
    XCTAssertNotEqual(app.otherElements["Gradient color 1"].frame.midX, before, "the gradient moved along")

    // Dark mode and more intensity and grain.
    app.buttons["Dark"].tap()
    // The sliders are at the bottom.
    // (Below the pad, which takes drags.)
    app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.8))
      .press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.3)))
    sleep(1)
    XCTAssertTrue(app.sliders["Grain"].isHittable, "sliders on screen")
    app.sliders["Intensity"].adjust(toNormalizedSliderPosition: 0.8)
    app.sliders["Grain"].adjust(toNormalizedSliderPosition: 1)
    sleep(1)
    snapshot("dark")

    app.buttons["ThemeDone"].tap()
    sleep(2)
    snapshot("sidebar")

    // Grey: the sliders go.
    app.buttons["Space menu"].firstMatch.tap()
    sleep(1)
    app.buttons["Edit Theme"].tap()
    XCTAssertTrue(app.otherElements["ThemePad"].waitForExistence(timeout: 3))
    app.buttons["Grey"].tap()
    sleep(1)
    XCTAssertFalse(app.sliders["Intensity"].exists, "no intensity for grey")
    XCTAssertFalse(app.otherElements["Gradient color 1"].exists, "grey has no gradient")
    app.buttons["Blue"].tap()
    app.buttons["Automatic"].tap()
    app.buttons["ThemeDone"].tap()
  }
}
