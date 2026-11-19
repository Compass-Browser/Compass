// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassShared

/// Copy Link's cleaning (docs/design/ios.md, "Clean copied links"): the
/// desktop's common/clean_url_unittest.cc, ported, with Firefox's lists as
/// the app bundles them (third_party/strip_on_share/).
struct CleanLinkTests {
  static let rules: CleanLink = {
    let lists = URL(fileURLWithPath: #filePath)
      .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
      .deletingLastPathComponent().deletingLastPathComponent()
      .appendingPathComponent("third_party/strip_on_share")
    return try! CleanLink(lists: [
      Data(contentsOf: lists.appendingPathComponent("MPL2/StripOnShare.json")),
      Data(contentsOf: lists.appendingPathComponent("LGPL/StripOnShareLGPL.json")),
    ])
  }()

  private func clean(_ link: String) -> String {
    Self.rules.clean(URL(string: link)!).absoluteString
  }

  @Test func globalParams() {
    // MPL2 list.
    #expect(clean("https://example.com/a?utm_source=x&utm_medium=y") == "https://example.com/a")
    #expect(clean("https://example.com/a?id=3&fbclid=abc&page=2") == "https://example.com/a?id=3&page=2")
    // Names compared without case.
    #expect(clean("https://example.com/?GCLID=1&UTM_Campaign=z&q=a") == "https://example.com/?q=a")
    // LGPL list.
    #expect(clean("https://example.com/?utm_newsletter_id=1&q=a") == "https://example.com/?q=a")
    // The fragment stays.
    #expect(clean("http://example.com/?mc_eid=2#top") == "http://example.com/#top")
  }

  @Test func keepsEverythingElse() {
    // Order, spelling, empty values and parameters without "=" are kept.
    #expect(
      clean("https://example.com/?b=2&a=%20x&flag&e=&utm_source=9") == "https://example.com/?b=2&a=%20x&flag&e=")
    #expect(clean("https://example.com/?utmost=1&source=2") == "https://example.com/?utmost=1&source=2")
    #expect(clean("https://example.com/#utm_source=x") == "https://example.com/#utm_source=x")
    let untouched = URL(string: "https://example.com/?q=a")!
    #expect(Self.rules.clean(untouched) == untouched)
  }

  @Test func siteParams() {
    // By registrable domain, subdomains too (schemelessSites).
    #expect(clean("https://open.spotify.com/track/1?si=abc") == "https://open.spotify.com/track/1")
    #expect(clean("https://youtu.be/abc?si=XyZ&t=42") == "https://youtu.be/abc?t=42")
    #expect(clean("https://www.youtube.com/watch?v=abc&si=XyZ") == "https://www.youtube.com/watch?v=abc")
    #expect(clean("https://x.com/a/status/1?s=20&t=abc") == "https://x.com/a/status/1?t=abc")
    // Only on their sites.
    #expect(clean("https://example.com/?si=1&s=2") == "https://example.com/?si=1&s=2")
    #expect(clean("https://notx.com/?s=2") == "https://notx.com/?s=2")
    // An exact host (hosts).
    #expect(clean("https://www.amazon.com.be/dp/1?pd_rd_i=x&k=y") == "https://www.amazon.com.be/dp/1?k=y")
  }

  @Test func nestedURLs() {
    #expect(
      clean("https://example.com/r?to=https%3A%2F%2Fa.example%2F%3Fid%3D1%26utm_source%3Dx&utm_medium=y")
        == "https://example.com/r?to=https%3A%2F%2Fa.example%2F%3Fid%3D1")
    // A value that isn't a URL, or a URL with nothing to clean, is kept as is.
    #expect(
      clean("https://example.com/r?to=https%3A%2F%2Fa.example%2F") == "https://example.com/r?to=https%3A%2F%2Fa.example%2F")
    // Only one level deep.
    #expect(
      clean("https://example.com/r?to=https%3A%2F%2Fa.example%2F%3Fu%3Dhttps%253A%252F%252Fb.example%252F%253Futm_source%253Dx")
        == "https://example.com/r?to=https%3A%2F%2Fa.example%2F%3Fu%3Dhttps%253A%252F%252Fb.example%252F%253Futm_source%253Dx")
  }

  @Test func onlyWebURLs() {
    #expect(clean("chrome://settings/?utm_source=x") == "chrome://settings/?utm_source=x")
    #expect(clean("file:///tmp/a.html?utm_source=x") == "file:///tmp/a.html?utm_source=x")
    #expect(clean("about:blank?utm_source=x") == "about:blank?utm_source=x")
  }

  @Test func emptyQueryGoes() {
    #expect(clean("https://example.com/?&utm_source=x") == "https://example.com/")
  }

  @Test func escapesAsChromium() {
    #expect(CleanLink.escapeQueryValue("a b/:?=&~*'()é") == "a+b%2F%3A%3F%3D%26~*'()%C3%A9")
  }

  @Test func setting() {
    let name = "CleanLinkTests.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: name)!
    defer { defaults.removePersistentDomain(forName: name) }
    #expect(CleanLink.isOn(defaults))
    defaults.set(false, forKey: CleanLink.removeTrackingKey)
    #expect(!CleanLink.isOn(defaults))
  }
}
