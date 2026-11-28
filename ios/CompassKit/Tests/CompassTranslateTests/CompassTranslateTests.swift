// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassTranslate

struct PageLanguageTests {
  @Test func normalizes() {
    #expect(PageLanguage.normalize("de-DE") == "de")
    #expect(PageLanguage.normalize("EN_us") == "en")
    #expect(PageLanguage.normalize("iw") == "he")
    #expect(PageLanguage.normalize("no") == "nb")
    #expect(PageLanguage.normalize("zh") == "zh-Hans")
    #expect(PageLanguage.normalize("zh-TW") == "zh-Hant")
    #expect(PageLanguage.normalize("zh-Hant-HK") == "zh-Hant")
    for none in ["", "und", "x-klingon", "1", "toolong"] {
      #expect(PageLanguage.normalize(none) == nil, "\(none)")
    }
  }

  @Test func offersOnlyWhatTheUserDoesntRead() {
    let preferred = ["it-IT", "en-GB"]
    #expect(PageLanguage.shouldOffer(source: "de", preferred: preferred, never: []))
    #expect(!PageLanguage.shouldOffer(source: "en", preferred: preferred, never: []))
    #expect(!PageLanguage.shouldOffer(source: "it", preferred: preferred, never: []))
    #expect(!PageLanguage.shouldOffer(source: "de", preferred: preferred, never: ["de"]))
    #expect(!PageLanguage.shouldOffer(source: nil, preferred: preferred, never: []))
    // Chinese in either script is Chinese.
    #expect(!PageLanguage.shouldOffer(source: "zh-Hant", preferred: ["zh-Hans-CN"], never: []))
    #expect(PageLanguage.target(source: "de", preferred: preferred) == "it")
    #expect(PageLanguage.target(source: "it", preferred: preferred) == "en")
  }

  @Test func detectsFromTheTextOverALeftoverAttribute() {
    let german = """
      Die Übersetzung läuft vollständig auf dem Gerät, und kein Text verlässt den Computer. \
      Klicken Sie hier, um fortzufahren, und lesen Sie die Hinweise zum Datenschutz.
      """
    #expect(PageLanguage.detect(langAttribute: "en", text: german) == "de")
    #expect(PageLanguage.detect(langAttribute: "de-AT", text: "") == "de")
    #expect(PageLanguage.detect(langAttribute: nil, text: "Hi") == nil)
  }
}

struct HTMLSegmentTests {
  @Test func translatesTextRunsAndKeepsMarkup() throws {
    let html = #"Klicken Sie <a data-compass-t="0">hier</a>, um <b data-compass-t="1">weiter</b> &amp; mehr."#
    let segment = HTMLSegment(html)
    #expect(segment.texts == ["Klicken Sie ", "hier", ", um ", "weiter", " & mehr."])
    let out = try #require(segment.html(with: ["Click", "here", ", to", "continue", "& more."]))
    #expect(out == #"Click <a data-compass-t="0">here</a>, to <b data-compass-t="1">continue</b> &amp; more."#)
    #expect(segment.html(with: ["one"]) == nil)
  }

  @Test func escapes() {
    #expect(HTMLSegment.unescape("a &lt;b&gt; &#233; &#x41; &nbsp;&bogus; &") == "a <b> é A \u{00A0}&bogus; &")
    #expect(HTMLSegment.escape("a<b & c\u{00A0}") == "a&lt;b &amp; c&nbsp;")
  }

  @Test func batchesSegmentsAsStrings() {
    let batch = PlainTextBatch([
      .init(text: "Guten Morgen", html: false),
      .init(text: #"Ein <i data-compass-t="0">Test</i>"#, html: true),
      .init(text: "Tschüss", html: false),
    ])
    #expect(batch.strings == ["Guten Morgen", "Ein ", "Test", "Tschüss"])
    let out = batch.translations(from: ["Good morning", "A", "test", nil])
    #expect(out == ["Good morning", #"A <i data-compass-t="0">test</i>"#, nil])
    // A failed run leaves its whole segment.
    #expect(batch.translations(from: ["x", nil, "y", "z"]) == ["x", nil, "z"])
  }
}

struct TranslationPrefsTests {
  @Test func alwaysAndNever() throws {
    let defaults = try #require(UserDefaults(suiteName: "TranslationPrefsTests-\(UUID())"))
    let prefs = TranslationPrefs(defaults: defaults)
    let url = URL(string: "https://www.Example.de/seite")
    #expect(prefs.shouldOffer(source: "de", url: url, preferred: ["en"]))
    prefs.setAlways("de", "en")
    #expect(prefs.always == ["de": "en"])
    prefs.setNever("de")
    #expect(prefs.always.isEmpty && prefs.never == ["de"])
    #expect(!prefs.shouldOffer(source: "de", url: url, preferred: ["en"]))
    prefs.setAlways("de", "en")
    #expect(prefs.never.isEmpty)
    prefs.setNeverSite(url)
    #expect(prefs.neverSites == ["example.de"])
    #expect(!prefs.shouldOffer(source: "fr", url: url, preferred: ["en"]))
    prefs.offers = false
    #expect(!prefs.shouldOffer(source: "fr", url: nil, preferred: ["en"]))
  }
}
