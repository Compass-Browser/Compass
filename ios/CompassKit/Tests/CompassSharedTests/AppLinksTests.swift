// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import XCTest

final class AppLinksTests: XCTestCase {
  private let page = URL(string: "https://www.example.com/article")!

  private func decide(
    _ link: String, page: URL? = URL(string: "https://www.example.com/article"), main: Bool = true,
    tap: Bool = true, incognito: Bool = false, setting: Bool = true, retry: Bool = false
  ) -> AppLinks.Decision {
    AppLinks.decide(
      .init(
        url: URL(string: link)!, page: page, isMainFrame: main, isUserTap: tap, isIncognito: incognito,
        openLinksInApps: setting, isRetry: retry))
  }

  func testBrowserSchemesLoad() {
    for link in [
      "http://example.com/", "https://example.com/x", "about:blank", "data:text/html,hi", "blob:https://example.com/1",
      "javascript:void(0)", "file:///tmp/a.html", "webkit-extension://abc/popup.html", "compass-reader://reader/reader.html",
    ] {
      XCTAssertEqual(decide(link, tap: false), .allow, link)
    }
  }

  func testOwnSchemesAreIgnored() {
    XCTAssertEqual(decide("compass://incognito"), .ignore)
    XCTAssertEqual(decide("compass-pair://x"), .ignore)
    XCTAssertEqual(decide("COMPASS://incognito", tap: false), .ignore)
  }

  func testSystemSchemesGoStraightToIOS() {
    for link in ["tel:+391234", "mailto:a@example.com", "sms:123", "facetime:a@example.com"] {
      XCTAssertEqual(decide(link), .openSystem(URL(string: link)!), link)
      XCTAssertEqual(decide(link, tap: false), .openSystem(URL(string: link)!), link)
    }
  }

  func testAppSchemesOpenOnTapAskOtherwise() {
    let zoom = URL(string: "zoommtg://zoom.us/join?confno=1")!
    XCTAssertEqual(decide(zoom.absoluteString), .openApp(zoom))
    XCTAssertEqual(decide(zoom.absoluteString, tap: false), .askToOpenApp(zoom))
    let spotify = URL(string: "spotify:track:1")!
    XCTAssertEqual(decide(spotify.absoluteString, tap: false), .askToOpenApp(spotify))
    XCTAssertEqual(decide("maps:?q=x", page: nil), .openApp(URL(string: "maps:?q=x")!))
  }

  func testAppSchemesIgnoreTheSetting() {
    let zoom = URL(string: "zoommtg://x")!
    XCTAssertEqual(decide(zoom.absoluteString, setting: false), .openApp(zoom))
    XCTAssertEqual(decide(zoom.absoluteString, incognito: true), .openApp(zoom))
  }

  func testSubframesNeverOpenApps() {
    XCTAssertEqual(decide("zoommtg://x", main: false), .ignore)
    XCTAssertEqual(decide("tel:123", main: false), .ignore)
    XCTAssertEqual(decide("zoommtg://x", main: false, tap: false), .ignore)
    XCTAssertEqual(decide("https://other.org/", main: false), .allow)
  }

  func testCrossSiteTapTriesUniversalLink() {
    let link = URL(string: "https://open.spotify.com/track/1")!
    XCTAssertEqual(decide(link.absoluteString), .tryUniversalLink(link))
    XCTAssertEqual(decide(link.absoluteString, page: URL(string: "http://example.com/")), .tryUniversalLink(link))
  }

  func testSameSiteStays() {
    XCTAssertEqual(decide("https://example.com/other"), .allow)
    XCTAssertEqual(decide("https://www.example.com/other"), .allow)
    XCTAssertEqual(decide("https://m.example.com/other"), .allow)
    XCTAssertEqual(decide("https://example.com/", page: URL(string: "https://news.example.com/")), .allow)
  }

  func testNoTapNoUniversalLink() {
    XCTAssertEqual(decide("https://open.spotify.com/track/1", tap: false), .allow)
  }

  func testSettingOff() {
    XCTAssertEqual(decide("https://open.spotify.com/track/1", setting: false), .allow)
  }

  func testIncognitoStaysInTheBrowser() {
    XCTAssertEqual(decide("https://open.spotify.com/track/1", incognito: true), .allow)
  }

  func testRetryLoadsInTheTab() {
    XCTAssertEqual(decide("https://open.spotify.com/track/1", retry: true), .allow)
  }

  func testOnlyHTTPSAndOnlyFromWebPages() {
    XCTAssertEqual(decide("http://open.spotify.com/track/1"), .allow)
    XCTAssertEqual(decide("https://open.spotify.com/track/1", page: nil), .allow)
    XCTAssertEqual(decide("https://open.spotify.com/track/1", page: URL(string: "about:blank")), .allow)
    XCTAssertEqual(decide("https://open.spotify.com/", main: false), .allow)
  }

  /// A shared Boost's link opens the preview instead of the page: tapped
  /// or not, Incognito too, in the main frame only.
  func testBoostShareLinks() {
    for link in [
      "https://compassbrowser.org/boost#abc-_1", "https://compassbrowser.org/boost/#abc-_1",
      "https://compassbrowser.org/boost.html#abc-_1", "https://www.compassbrowser.org/boost#abc-_1",
      "HTTPS://CompassBrowser.org/boost#abc-_1", "https://compassbrowser.org/boost?ref=x#abc-_1",
    ] {
      XCTAssertEqual(decide(link), .importBoost(fragment: "abc-_1"), link)
      XCTAssertEqual(decide(link, tap: false, incognito: true), .importBoost(fragment: "abc-_1"), link)
      XCTAssertEqual(decide(link, page: nil), .importBoost(fragment: "abc-_1"), link)
    }
    XCTAssertEqual(decide("https://compassbrowser.org/boost#abc", main: false), .allow)
    for link in [
      "https://compassbrowser.org/boost", "https://compassbrowser.org/boost#", "http://compassbrowser.org/boost#abc",
      "https://compassbrowser.org/boosts#abc", "https://compassbrowser.org/boost/x#abc",
      "https://compassbrowser.org/#abc", "https://compassbrowser.org:8443/boost#abc",
      "https://user@compassbrowser.org/boost#abc", "https://compassbrowser.org.example.com/boost#abc",
      "https://example.com/boost#abc", "compass://boost#abc",
    ] {
      XCTAssertNil(AppLinks.boostShareFragment(URL(string: link)!), link)
    }
    XCTAssertEqual(decide("https://compassbrowser.org/boost", tap: false), .allow)
  }

  func testSameSite() {
    XCTAssertTrue(AppLinks.sameSite(URL(string: "https://WWW.Example.com/")!, page))
    XCTAssertTrue(AppLinks.sameSite(URL(string: "https://example.com./")!, page))
    XCTAssertFalse(AppLinks.sameSite(URL(string: "https://notexample.com/")!, page))
    XCTAssertFalse(AppLinks.sameSite(URL(string: "https://a.example.com/")!, URL(string: "https://b.example.com/")!))
  }

  func testRoomInviteLinksOpenTheJoinPreview() {
    let invite = URL(string: "https://compassbrowser.org/join#eyJlIjoxfQ")!
    let tap = AppLinks.Navigation(url: invite, page: URL(string: "https://mail.example/"), isMainFrame: true, isUserTap: true)
    XCTAssertEqual(AppLinks.decide(tap), .joinRoom(invite))
    // Not in a frame, not in Incognito, not without a fragment or elsewhere.
    var frame = tap
    frame.isMainFrame = false
    XCTAssertNotEqual(AppLinks.decide(frame), .joinRoom(invite))
    var incognito = tap
    incognito.isIncognito = true
    XCTAssertNotEqual(AppLinks.decide(incognito), .joinRoom(invite))
    for link in ["https://compassbrowser.org/join", "https://evil.example/join#x", "http://compassbrowser.org/join#x",
      "https://compassbrowser.org/joinx#x", "https://compassbrowser.org:444/join#x"]
    {
      XCTAssertFalse(AppLinks.isRoomInvite(URL(string: link)!), link)
    }
    XCTAssertTrue(AppLinks.isRoomInvite(URL(string: "https://www.compassbrowser.org/join.html#x")!))
  }
}
