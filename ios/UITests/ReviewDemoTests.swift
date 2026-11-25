// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import XCTest

/// App Review's screen recording (guideline 2.1): the typical flow from
/// launch, paced for a person to follow. Run on a physical iPhone while its
/// screen is recorded (QuickTime's New Movie Recording, over USB). Not part
/// of the regular suite: it skips unless REVIEW_DEMO is set.
///
/// Environment (pass as TEST_RUNNER_<name> to xcodebuild):
/// - REVIEW_DEMO=1 to run.
/// - REVIEW_AI_KEY: an Anthropic API key for the AI part (skipped without).
/// - REVIEW_INVITE: a sync invite link for the sync part (skipped without).
final class ReviewDemoTests: XCTestCase {
  private var app: XCUIApplication!

  /// A pause long enough to see what just happened.
  private func beat(_ seconds: Double = 2) {
    Thread.sleep(forTimeInterval: seconds)
  }

  /// Taps the first existing element, or records what was on screen.
  @discardableResult
  private func tap(_ element: XCUIElement, _ what: String, timeout: TimeInterval = 10) -> Bool {
    guard element.waitForExistence(timeout: timeout) else {
      let shot = XCTAttachment(string: app.debugDescription)
      shot.name = "missing \(what)"
      shot.lifetime = .keepAlways
      add(shot)
      XCTFail("missing \(what)")
      return false
    }
    element.tap()
    return true
  }

  private func type(_ text: String) {
    // Slow enough to read as typing.
    for chunk in text.chunked(6) {
      app.typeText(chunk)
      Thread.sleep(forTimeInterval: 0.08)
    }
  }

  private func openSettings() {
    tap(app.buttons["Menu"], "menu")
    beat(1)
    tap(app.buttons["MenuSettings"], "menu settings")
    beat(1.5)
  }

  private func closeSettings() {
    tap(app.buttons["Done"].firstMatch, "settings done")
    beat(1)
  }

  private func back() {
    app.navigationBars.buttons.element(boundBy: 0).tap()
    beat(1)
  }

  private func scrollTo(_ element: XCUIElement) {
    var tries = 0
    while !element.isHittable, tries < 8 {
      app.swipeUp(velocity: .slow)
      tries += 1
    }
  }

  func testReviewDemo() throws {
    let env = ProcessInfo.processInfo.environment
    try XCTSkipUnless(env["REVIEW_DEMO"] != nil, "App Review demo: set REVIEW_DEMO")
    continueAfterFailure = true
    let key = env["REVIEW_AI_KEY"].flatMap { $0.isEmpty ? nil : $0 }
    let invite = env["REVIEW_INVITE"].flatMap { $0.isEmpty ? nil : $0 }

    // The recording starts on the Home Screen; the launch is in it.
    XCUIDevice.shared.press(.home)
    beat(3)
    app = XCUIApplication()
    // The demo sidebar, and never the Keychain's sync chain: the developer's
    // own devices' data stays out of the video.
    app.launchArguments = ["-CompassDemo", "YES", "-CompassNoSync", "YES"]
    app.launch()
    beat(4)

    // Search from the command bar and open a page.
    tap(app.buttons["BarNewTab"], "new tab")
    beat(1)
    type("en.wikipedia.org/wiki/Compass")
    beat(1)
    app.typeText("\n")
    beat(5)

    // Reader.
    if app.buttons["BarReader"].waitForExistence(timeout: 5) {
      app.buttons["BarReader"].tap()
      beat(3)
      app.swipeUp(velocity: .slow)
      beat(2)
      app.buttons["BarReader"].firstMatch.tap()
      beat(2)
    }

    // Pin the tab.
    tap(app.buttons["Menu"], "menu")
    beat(1.5)
    tap(app.buttons["Pin"], "pin")
    beat(2)
    // Close the menu.
    app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.15)).tap()
    beat(1.5)

    // The sidebar: spaces, pinned tabs, folders, Today.
    let bar = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.91))
    bar.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.1)),
              withVelocity: .fast, thenHoldForDuration: 0)
    beat(3)
    // Next space and back, swiping the sidebar's list.
    let mid = app.coordinate(withNormalizedOffset: CGVector(dx: 0.85, dy: 0.5))
    let left = app.coordinate(withNormalizedOffset: CGVector(dx: 0.15, dy: 0.5))
    mid.press(forDuration: 0.05, thenDragTo: left, withVelocity: .fast, thenHoldForDuration: 0)
    beat(2.5)
    left.press(forDuration: 0.05, thenDragTo: mid, withVelocity: .fast, thenHoldForDuration: 0)
    beat(2.5)
    let grabber = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.085))
    grabber.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.98)))
    beat(2)

    // AI with the reviewer's key.
    if let key {
      openSettings()
      tap(app.buttons["AI"].firstMatch, "settings AI")
      beat(1.5)
      if app.buttons["Remove Key"].exists {
        app.buttons["Remove Key"].tap()
        beat(1)
      }
      tap(app.secureTextFields["ClaudeKeyField"], "key field")
      app.typeText(key)
      beat(1)
      tap(app.buttons["Save Key"], "save key")
      beat(2)
      back()
      closeSettings()

      tap(app.buttons["Address"], "address")
      beat(1)
      type("What is this page about, in three sentences?")
      beat(1)
      if app.switches["CommandBarAskAboutPage"].exists,
         app.switches["CommandBarAskAboutPage"].value as? String == "0" {
        app.switches["CommandBarAskAboutPage"].tap()
        beat(1)
      }
      tap(app.buttons["CommandBarAsk"], "ask")
      beat(15)
      app.swipeDown(velocity: .slow)
      beat(2)
    }

    // Sync with an invite, then leave.
    if let invite {
      openSettings()
      let row = app.buttons["SyncInvite"]
      scrollTo(row)
      tap(row, "I Have an Invite")
      beat(1.5)
      tap(app.textViews["InviteLink"].exists ? app.textViews["InviteLink"] : app.textFields["InviteLink"], "invite field")
      type(invite)
      beat(1)
      tap(app.buttons["Join and Start Sync"], "join")
      beat(12)
      let leave = app.buttons["Leave Sync"]
      scrollTo(leave)
      beat(3)
      tap(leave, "leave sync")
      beat(1.5)
      tap(app.buttons.matching(NSPredicate(format: "label == 'Leave Sync'")).element(boundBy: 1), "confirm leave")
      beat(3)
      closeSettings()
    }

    // An incognito tab: a long press on the bar's +.
    if app.buttons["BarNewTab"].waitForExistence(timeout: 5) {
      app.buttons["BarNewTab"].press(forDuration: 1.2)
      beat(1)
      tap(app.buttons["New Incognito Tab"], "new incognito tab")
      beat(1)
      type("compassbrowser.org")
      app.typeText("\n")
      beat(5)
    }
  }
}

private extension String {
  func chunked(_ size: Int) -> [String] {
    stride(from: 0, to: count, by: size).map {
      let start = index(startIndex, offsetBy: $0)
      return String(self[start..<(index(start, offsetBy: size, limitedBy: endIndex) ?? endIndex)])
    }
  }
}
