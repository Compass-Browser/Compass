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

struct BergamotPacksTests {
  /// A manifest of German's two directions, each a model and a vocabulary,
  /// served by a fake fetch; "decompressing" copies, so both hashes are of
  /// the same bytes.
  static func fixture() throws -> (BergamotPacks, URL, @Sendable () -> Int) {
    let root = FileManager.default.temporaryDirectory.appending(path: "BergamotPacksTests-\(UUID())")
    var files: [String: Data] = [:]
    func file(_ type: String, _ name: String, _ text: String) -> TranslationManifest.File {
      let data = Data(text.utf8)
      files[name] = data
      let hash = SHA256Hex(data)
      return .init(type: type, location: name, size: Int64(data.count), hash: hash,
                   decompressedSize: Int64(data.count), decompressedHash: hash)
    }
    let manifest = TranslationManifest(attachments: "https://assets.test/translation/", directions: [
      "de-en": .init(files: [file("model", "de-en.zst", "model de en"), file("vocab", "v1.zst", "vocab")]),
      "en-de": .init(files: [file("model", "en-de.zst", "model en de!"), file("vocab", "v2.zst", "vocab")]),
      "nn-en": .init(files: [file("model", "nn-en.zst", "nn")]),
    ])
    let served = files
    let count = Counter()
    let packs = BergamotPacks(
      root: root, manifest: manifest,
      fetch: { url, progress in
        count.add()
        guard let data = served[url.lastPathComponent] else { throw URLError(.fileDoesNotExist) }
        let temp = FileManager.default.temporaryDirectory.appending(path: UUID().uuidString)
        try data.write(to: temp)
        progress(Int64(data.count))
        return temp
      },
      decompress: { from, to in (try? FileManager.default.copyItem(at: from, to: to)) != nil })
    return (packs, root, { count.value })
  }

  @Test func plansPairs() throws {
    let (packs, _, _) = try Self.fixture()
    let manifest = packs.manifest
    #expect(manifest.languages == ["de", "nn"])
    #expect(manifest.translates("de", "en") && manifest.translates("en", "de"))
    #expect(!manifest.translates("en", "nn"))  // into English only
    #expect(manifest.translates("nn", "de"))  // through English
    #expect(TranslationManifest.packs("de", "en") == ["de"])
    #expect(TranslationManifest.packs("nn", "de") == ["nn", "de"])
    #expect(manifest.directions(ofPack: "nn") == ["nn-en"])
    #expect(packs.downloadSize(["de"]) == 11 + 5 + 12 + 5)
  }

  @Test func installsChecksAndUninstalls() async throws {
    let (packs, root, fetched) = try Self.fixture()
    defer { try? FileManager.default.removeItem(at: root) }
    #expect(packs.missing("de", "en") == ["de"])
    let reports = Reports()
    try await packs.install(["de"]) { reports.add($0, $1) }
    #expect(packs.installed == ["de"])
    #expect(packs.missing("de", "en").isEmpty)
    let model = try String(contentsOf: root.appending(path: "de/en-de/model.bin"), encoding: .utf8)
    #expect(model == "model en de!")
    #expect(FileManager.default.fileExists(atPath: root.appending(path: "de/de-en/vocab.spm").path))
    #expect(fetched() == 4)
    #expect(reports.last.map { $0 == (33, 33) } == true)
    #expect(reports.isRising)
    #expect(packs.packages.map(\.0) == ["de"])
    // Installed: nothing fetched again.
    try await packs.install(["de"]) { _, _ in }
    #expect(fetched() == 4)
    packs.uninstall("de")
    #expect(packs.installed.isEmpty)
  }

  @Test func refusesAFileThatDoesntMatch() async throws {
    let (good, root, _) = try Self.fixture()
    defer { try? FileManager.default.removeItem(at: root) }
    var directions = good.manifest.directions
    let model = try #require(directions["de-en"]?.files.first)
    directions["de-en"] = .init(files: [.init(
      type: model.type, location: model.location, size: model.size, hash: String(repeating: "0", count: 64),
      decompressedSize: model.decompressedSize, decompressedHash: model.decompressedHash)])
    let packs = BergamotPacks(
      root: root, manifest: TranslationManifest(attachments: good.manifest.attachments, directions: directions),
      fetch: { url, _ in
        let temp = FileManager.default.temporaryDirectory.appending(path: UUID().uuidString)
        try Data("model de en".utf8).write(to: temp)
        return temp
      },
      decompress: { _, _ in true })
    await #expect(throws: BergamotPacks.Failure.mismatch(model.location)) {
      try await packs.install(["de"]) { _, _ in }
    }
    #expect(packs.installed.isEmpty)
    // Nothing left behind but the root.
    #expect((try? FileManager.default.contentsOfDirectory(atPath: root.path))?.isEmpty ?? true)
  }
}

import CryptoKit

func SHA256Hex(_ data: Data) -> String {
  SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

final class Counter: @unchecked Sendable {
  private let lock = NSLock()
  private var count = 0
  func add() { lock.withLock { count += 1 } }
  var value: Int { lock.withLock { count } }
}

final class Reports: @unchecked Sendable {
  private let lock = NSLock()
  private var all: [(Int64, Int64)] = []
  func add(_ a: Int64, _ b: Int64) { lock.withLock { all.append((a, b)) } }
  var last: (Int64, Int64)? { lock.withLock { all.last } }
  var isRising: Bool { lock.withLock { zip(all, all.dropFirst()).allSatisfy { $0.0 <= $1.0 } } }
}
