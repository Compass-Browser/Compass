// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassBoosts

/// The desktop's DevModeTest, in Swift.
struct DevModeSitesTests {
  @Test func normalizes() {
    #expect(DevModeSites.normalize("localhost") == "localhost")
    #expect(DevModeSites.normalize("  LocalHost:3000 ") == "localhost")
    #expect(DevModeSites.normalize("http://localhost:3000/app?x=1") == "localhost")
    #expect(DevModeSites.normalize("https://www.Example.com/a") == "example.com")
    #expect(DevModeSites.normalize("*.example.com") == "*.example.com")
    #expect(DevModeSites.normalize("127.0.0.1:8080") == "127.0.0.1")
  }

  @Test func refuses() {
    for input in ["", "  ", "*.", "*", "*.com", "ftp://example.com", "chrome://settings",
                  "file:///etc/hosts", "*.127.0.0.1"] {
      #expect(DevModeSites.normalize(input) == nil, "\(input)")
    }
  }

  @Test func addsAndRemoves() {
    var sites = DevModeSites()
    #expect(!sites.contains(URL(string: "http://localhost:3000/")))
    #expect(sites.add("localhost:3000") == "localhost")
    #expect(sites.add("http://localhost") == "localhost")
    #expect(sites.add("*.example.com") == "*.example.com")
    #expect(sites.add("chrome://settings") == nil)
    #expect(sites.patterns == ["localhost", "*.example.com"])

    #expect(sites.contains(URL(string: "http://localhost:5173/")))
    #expect(sites.contains(URL(string: "https://app.example.com/x")))
    #expect(!sites.contains(URL(string: "https://example.org/")))
    #expect(!sites.contains(URL(string: "about:blank")))

    sites.remove("localhost")
    #expect(!sites.contains(URL(string: "http://localhost:5173/")))
    #expect(sites.patterns == ["*.example.com"])
  }

  @Test func turnsOnAndOffForAPage() throws {
    var sites = DevModeSites()
    let page = try #require(URL(string: "https://www.example.com:8443/docs"))
    sites.set(page, enabled: true)
    #expect(sites.patterns == ["example.com"])
    #expect(sites.contains(page))
    sites.set(page, enabled: true)
    #expect(sites.patterns.count == 1)
    sites.set(page, enabled: false)
    #expect(!sites.contains(page))
    #expect(sites.patterns.isEmpty)

    // Off removes every pattern that matches the page.
    sites.add("*.example.com")
    sites.add("app.example.com")
    sites.add("localhost")
    sites.set(try #require(URL(string: "https://app.example.com/")), enabled: false)
    #expect(sites.patterns == ["localhost"])
  }

  @Test func dropsBadSavedPatterns() {
    #expect(DevModeSites(["localhost", "not a host", "*.example.com"]).patterns
      == ["localhost", "*.example.com"])
  }
}
