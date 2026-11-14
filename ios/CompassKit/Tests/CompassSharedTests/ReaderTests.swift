// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassShared

/// Reader mode's pure parts (docs/design/reader-mode.md, "iOS").
struct ReaderSettingsTests {
  private func defaults() -> UserDefaults {
    let name = "ReaderSettingsTests.\(UUID().uuidString)"
    let defaults = UserDefaults(suiteName: name)!
    defaults.removePersistentDomain(forName: name)
    return defaults
  }

  /// reader_view.js's kDefaults.
  @Test func defaultsAreTheDesktops() {
    let settings = ReaderSettings(defaults: defaults())
    #expect(settings == ReaderSettings())
    #expect(settings.json == #"{"font":"serif","size":19,"theme":"auto","width":"medium"}"#)
  }

  @Test func roundTripsAsTheDesktopsJSON() {
    let store = defaults()
    var settings = ReaderSettings()
    settings.font = "sans"
    settings.size = 24
    settings.width = "wide"
    settings.theme = "sepia"
    settings.save(to: store)
    #expect(store.string(forKey: "compass.reader.settings") == #"{"font":"sans","size":24,"theme":"sepia","width":"wide"}"#)
    #expect(ReaderSettings(defaults: store) == settings)
  }

  /// reader_view.js's validSettings(): each value on its own.
  @Test func unknownValuesFallBack() {
    let settings = ReaderSettings(["font": "comic", "size": 18, "width": "wide", "theme": "neon"] as [String: Any])
    #expect(settings.font == "serif")
    #expect(settings.size == 19)
    #expect(settings.width == "wide")
    #expect(settings.theme == "auto")
    #expect(ReaderSettings(["size": true]).size == 19)
    #expect(ReaderSettings(["size": 21.5]).size == 19)
    #expect(ReaderSettings(["size": 21.0]).size == 21)
    #expect(ReaderSettings("nonsense") == ReaderSettings())
    let store = defaults()
    store.set("{not json", forKey: ReaderSettings.key)
    #expect(ReaderSettings(defaults: store) == ReaderSettings())
  }

  /// The reader page's script hands them over (and the sizes are the JS's).
  @Test func hostScript() {
    var settings = ReaderSettings()
    settings.theme = "dark"
    let script = ReaderPageFiles.hostScript(settings: settings, handler: "compassReader")
    #expect(script.contains(#"const settings = {"font":"serif","size":19,"theme":"dark","width":"medium"};"#))
    #expect(script.contains("window.webkit.messageHandlers.compassReader.postMessage"))
    #expect(script.contains("globalThis.compassReaderHost"))
    #expect(ReaderSettings.sizes == [14, 15, 17, 19, 21, 24, 28, 32])
  }
}

struct ReaderResultTests {
  @Test func parseResult() {
    let json = #"{"ok":true,"title":"A title","content":"<p>Hi</p>","length":2}"#
    let result = ReaderResult(json: json)
    #expect(result.ok)
    #expect(result.title == "A title")
    #expect(result.json == json)
  }

  @Test func checkResult() {
    #expect(ReaderResult(json: #"{"ok":true,"readerable":true}"#).readerable)
    #expect(!ReaderResult(json: #"{"ok":true,"readerable":false}"#).readerable)
  }

  /// Nothing readable becomes an error the view can show.
  @Test func failures() {
    let error = ReaderResult(json: #"{"ok":false,"error":"No article found on this page."}"#)
    #expect(!error.ok)
    #expect(error.error == "No article found on this page.")
    for text in [nil, "", "null", "[1]", "{oops"] as [String?] {
      let result = ReaderResult(json: text)
      #expect(!result.ok)
      #expect(ReaderResult(json: result.json).error == "This page has no article.")
    }
  }
}

struct ReaderPageFilesTests {
  @Test func servesOnlyTheReaderFiles() {
    #expect(Set(ReaderPageFiles.files.keys) == ["/reader.html", "/reader.css", "/reader.js", "/reader_view.js"])
    #expect(ReaderPageFiles.pageURL.absoluteString == "compass-reader://reader/reader.html")
  }

  @Test func policyRunsOnlyItsOwnScripts() {
    let policy = ReaderPageFiles.contentSecurityPolicy
    #expect(policy.contains("default-src 'none'"))
    #expect(policy.contains("script-src 'self'"))
    #expect(policy.contains("frame-src 'none'"))
    #expect(!policy.contains("unsafe"))
    let page = ReaderPageFiles.page("<html><head><title>Reader</title></head></html>")
    #expect(page.hasPrefix(#"<html><head>"# + "\n" + #"<meta http-equiv="Content-Security-Policy" content="default-src 'none';"#))
    #expect(page.hasSuffix("<title>Reader</title></head></html>"))
  }
}

struct ReaderLinkTests {
  let page = URL(string: "https://example.com/news/story?id=1")!

  @Test func linksOpenInTheTab() {
    let other = URL(string: "https://example.com/other")!
    #expect(ReaderLink.decide(other, page: page) == .openInTab(other))
    let spot = URL(string: "https://example.com/news/story?id=1#notes")!
    #expect(ReaderLink.decide(spot, page: page) == .openInTab(spot))
  }

  @Test func originalPageLeaves() {
    #expect(ReaderLink.decide(page, page: page) == .leave)
  }

  @Test func readerPageStays() {
    #expect(ReaderLink.decide(ReaderPageFiles.pageURL, page: page) == .stay)
    #expect(ReaderLink.decide(URL(string: "about:blank")!, page: page) == .stay)
  }

  @Test func mailGoesToTheSystem() {
    let mail = URL(string: "mailto:someone@example.com")!
    #expect(ReaderLink.decide(mail, page: page) == .system(mail))
  }

  @Test func othersAreIgnored() {
    #expect(ReaderLink.decide(URL(string: "javascript:alert(1)")!, page: page) == .ignore)
    #expect(ReaderLink.decide(URL(string: "data:text/html,hi")!, page: page) == .ignore)
    #expect(ReaderLink.decide(URL(string: "file:///etc/hosts")!, page: page) == .ignore)
  }
}
