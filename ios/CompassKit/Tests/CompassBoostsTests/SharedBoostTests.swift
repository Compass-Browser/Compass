// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassBoosts

/// Sharing Boosts (docs/design/boosts.md, "Sharing"): the JSON, the link's
/// raw DEFLATE and base64url, the file, and what adding one checks.
struct SharedBoostTests {
  /// Made with Python's zlib (raw DEFLATE, wbits -15) and
  /// base64.urlsafe_b64encode without padding: another implementation's
  /// output, so this checks the format and not only a round trip.
  static let vectorJSON =
    #"{"v":1,"name":"Quiet GitHub","match":"github.com/settings","css":"header { display: none !important; }","js":"document.title = \"Settings\";","js_world":"main"}"#
  static let vectorFragment =
    "LczBCsIwEEXRXxlnXSpuW1zrVtwWSpoM7UgyKclEkdJ_N4rbdw9vwyd2pwbFBMIOb4VJ4cJ6LRM2GIzapc4z61Km1sZwzKTKMudabc61LWQcJdjAcV69eXcgUQgOHNaY1Ij2sFf8-FoXbQkk2iqrJzjDgPf_34D9T42vmLyrNhgW3D8"
  static let vector = SharedBoost(
    name: "Quiet GitHub", match: "github.com/settings", css: "header { display: none !important; }",
    js: #"document.title = "Settings";"#, jsWorld: .main)

  @Test func fixedVectorDecodes() throws {
    #expect(SharedBoost.decode(fragment: Self.vectorFragment) == .success(Self.vector))
    // This implementation writes the same JSON for it (key order, escapes).
    #expect(String(decoding: Self.vector.json(), as: UTF8.self) == Self.vectorJSON)
  }

  @Test func roundTrips() throws {
    let boosts = [
      Self.vector,
      SharedBoost(name: "Dark", match: "*.example.com", css: "body { background: #000 }"),
      SharedBoost(name: "Ünïcödé “quotes” / slashes \\ \n tabs\t", match: "example.org/a", js: "alert('✓')"),
    ]
    for boost in boosts {
      let link = try #require(boost.link())
      #expect(link.absoluteString.hasPrefix(SharedBoost.linkPrefix))
      let fragment = String(link.absoluteString.dropFirst(SharedBoost.linkPrefix.count))
      #expect(!fragment.contains("=") && !fragment.contains("+") && !fragment.contains("/"))
      #expect(SharedBoost.decode(fragment: fragment) == .success(boost))
      // The file: the same JSON, pretty-printed.
      #expect(SharedBoost.decode(file: boost.fileData) == .success(boost))
      let text = String(decoding: boost.fileData, as: UTF8.self)
      #expect(text.hasPrefix("{\n  \"v\": 1,\n  \"name\": "))
    }
  }

  @Test func absentAndUnknownKeys() {
    let json = #"{"v":1,"name":"N","match":"example.com","css":"a{}","future":{"x":[1,2]},"author":"claude","id":"x"}"#
    #expect(
      SharedBoost.decode(json: Data(json.utf8)) == .success(SharedBoost(name: "N", match: "example.com", css: "a{}")))
    // No js: no js_world either.
    let written = String(decoding: SharedBoost(name: "N", match: "example.com", css: "a{}").json(), as: UTF8.self)
    #expect(written == #"{"v":1,"name":"N","match":"example.com","css":"a{}"}"#)
  }

  @Test func otherVersionsAreRefused() {
    for v in ["2", "0", "1.5", "100"] {
      let json = #"{"v":\#(v),"name":"N","match":"example.com","css":"a{}"}"#
      #expect(SharedBoost.decode(json: Data(json.utf8)) == .failure(.newerVersion), "\(v)")
    }
    #expect(SharedBoost.Failure.newerVersion.description.contains("made by a newer Compass"))
    for bad in [
      #"{"name":"N","match":"example.com"}"#, #"{"v":"1","match":"example.com"}"#, #"{"v":true,"match":"example.com"}"#,
      #"{"v":1,"name":"N"}"#, #"{"v":1,"match":5}"#, #"{"v":1,"match":"example.com","css":["a"]}"#, "[1]", "",
      "not json",
    ] {
      #expect(SharedBoost.decode(json: Data(bad.utf8)) == .failure(.notABoost), "\(bad)")
    }
    #expect(
      SharedBoost.decode(json: Data(#"{"v":1,"match":"example.com","js":"x","js_world":"page"}"#.utf8))
        == .failure(.invalid(.badWorld("page"))))
  }

  @Test func decompressionIsCapped() throws {
    // 600 KB of JSON compresses to a few hundred bytes; inflating stops at
    // 512 KB.
    let big = SharedBoost(name: "Big", match: "example.com", css: String(repeating: " ", count: 600 * 1024))
    let link = try #require(big.link())
    #expect(link.absoluteString.utf8.count < 4096)
    let fragment = String(link.absoluteString.dropFirst(SharedBoost.linkPrefix.count))
    #expect(SharedBoost.decode(fragment: fragment) == .failure(.tooLarge))
    let deflated = try #require(SharedBoost.fromBase64URL(fragment))
    #expect(SharedBoost.inflate(deflated, limit: SharedBoost.maxJSONBytes) == .failure(.tooLarge))
    // A file that large isn't read either.
    #expect(SharedBoost.decode(file: big.fileData) == .failure(.tooLarge))
    // A fragment too long to be anything this size.
    #expect(SharedBoost.decode(fragment: String(repeating: "A", count: 800 * 1024)) == .failure(.tooLarge))
  }

  @Test func unreadableLinks() throws {
    for bad in ["", "!!!!", "abc def", "a+b/c", "A", "%41%42", "QUJD"] {
      #expect(SharedBoost.decode(fragment: bad) == .failure(.unreadable), "\(bad)")
    }
    // zlib's header (not raw DEFLATE) isn't read.
    let zlib =
      "eJwtzMEKwjAQRdFfGWddKm5bXOtW3BZKmgztSDIpyUSR0n83itt3D2_DJ3anBsUEwg5vhUnhwnotEzYYjNqlzjPrUqbWxnDMpMoy51ptzrUtZBwl2MBxXr15dyBRCA4c1pjUiPawV_z4WhdtCSTaKqsnOMOA9__fgP1Pja-YvKs2GBbcP5dCNWs"
    #expect(SharedBoost.decode(fragment: zlib) == .failure(.unreadable))
    // Cut short.
    #expect(SharedBoost.decode(fragment: String(Self.vectorFragment.prefix(60))) == .failure(.unreadable))
    // Valid DEFLATE, not a Boost.
    let text = SharedBoost.base64URL(try #require(SharedBoost.deflate(Data("hello".utf8))))
    #expect(SharedBoost.decode(fragment: text) == .failure(.notABoost))
    // Padding is tolerated.
    #expect(SharedBoost.decode(fragment: Self.vectorFragment + "=") == .success(Self.vector))
  }

  @Test func addingFollowsTheSaveRules() throws {
    let now = Date(timeIntervalSince1970: 1_800_000_000)
    var list = BoostList()
    let existing = try list.resolve(BoostDraft(name: "Mine", match: "github.com/settings", css: "a{}"), author: .user).get()
    list.put(existing)

    // A new Boost (never the one already there), enabled, by "shared",
    // the pattern normalized.
    var shared = Self.vector
    shared.match = "https://GitHub.com/settings*"
    let added = try list.resolveShared(shared, now: now).get()
    #expect(added.id != existing.id && BoostList.isValid(added))
    #expect(added.author == .shared && added.enabled)
    #expect(added.match == "github.com/settings" && added.name == "Quiet GitHub" && added.jsWorld == .main)
    #expect(added.createTime == Boost.windowsMicros(now))
    #expect(list.put(added) && list.boosts.count == 2 && list.find(existing.id) == existing)

    // An empty name is the site, as in the editor.
    #expect(try list.resolveShared(SharedBoost(name: " ", match: "example.com", css: "a{}")).get().name == "example.com")
    // Refused as a save would be.
    #expect(list.resolveShared(SharedBoost(name: "N", match: "example.com:8080", css: "a{}")) == .failure(.invalid(.badMatch("example.com:8080"))))
    #expect(
      list.resolveShared(SharedBoost(name: String(repeating: "n", count: 101), match: "example.com", css: "a{}"))
        == .failure(.invalid(.nameTooLong)))
    let tooBig = SharedBoost(
      name: "N", match: "example.com", css: String(repeating: "a", count: 200 * 1024),
      js: String(repeating: "b", count: 57 * 1024))
    #expect(list.resolveShared(tooBig) == .failure(.invalid(.tooLarge)))
    // A link that size still decodes: the size is the save's to refuse.
    #expect(SharedBoost.decode(fragment: String(try #require(tooBig.link()).absoluteString.dropFirst(SharedBoost.linkPrefix.count))) == .success(tooBig))
    #expect(list.resolveShared(SharedBoost(name: "N", match: "example.com")) == .failure(.empty))
    var full = BoostList()
    for i in 0..<BoostList.maxBoosts {
      full.put(try full.resolve(BoostDraft(name: "\(i)", match: "example.com", css: "a{}"), author: .user).get())
    }
    #expect(full.resolveShared(Self.vector) == .failure(.invalid(.full)))
  }

  /// Claude changing a shared Boost's JavaScript still asks: approval is
  /// about the code, not who made it.
  @Test func claudeStillAsksForSharedJavaScript() throws {
    var list = BoostList()
    let added = try list.resolveShared(Self.vector).get()
    list.put(added)
    let changed = try list.resolve(BoostDraft(id: added.id, name: added.name, match: added.match, js: "alert(1)"), author: .claude).get()
    #expect(changed.author == .shared)
    #expect(BoostList.needsApproval(changed, before: added))
    let cssOnly = try list.resolve(BoostDraft(id: added.id, name: added.name, match: added.match, css: "b{}"), author: .claude).get()
    #expect(!BoostList.needsApproval(cssOnly, before: added))
    #expect(BoostList.deleteNeedsApproval(added))
  }

  @Test func sharedAuthorPersists() throws {
    var list = BoostList()
    list.put(try list.resolveShared(Self.vector).get())
    #expect(BoostList.decode(list.encoded())?.boosts.first?.author == .shared)
  }

  @Test func fileNames() {
    #expect(SharedBoost(name: "Quiet GitHub", match: "github.com").fileName == "Quiet GitHub.compassboost")
    #expect(SharedBoost(name: "a/b:c", match: "x.com").fileName == "a-b-c.compassboost")
    #expect(SharedBoost(name: "..", match: "x.com").fileName == "Boost.compassboost")
  }

  // MARK: The desktop's vectors

  /// common/boosts/share_vectors.json, shared with the desktop, when it's
  /// in the checkout (or at $COMPASS_BOOST_SHARE_VECTORS).
  static var sharedVectorsURL: URL? {
    var candidates: [URL] = []
    if let path = ProcessInfo.processInfo.environment["COMPASS_BOOST_SHARE_VECTORS"] {
      candidates.append(URL(fileURLWithPath: path))
    }
    // Tests/CompassBoostsTests/ → the package → ios/ → the repository.
    let repo = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
      .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    candidates.append(repo.appendingPathComponent("common/boosts/share_vectors.json"))
    return candidates.first { FileManager.default.fileExists(atPath: $0.path) }
  }

  @Test(.enabled(if: sharedVectorsURL != nil, "common/boosts/share_vectors.json isn't in this checkout"))
  func desktopVectorsDecode() throws {
    let data = try Data(contentsOf: try #require(Self.sharedVectorsURL))
    let root = try JSONSerialization.jsonObject(with: data)
    // An array of vectors, or an object holding one.
    let entries =
      (root as? [[String: Any]])
      ?? (root as? [String: Any])?.values.compactMap { $0 as? [[String: Any]] }.first ?? []
    var checked = 0
    for entry in entries {
      guard let fragment = (entry["fragment"] as? String).map(Self.stripPrefix) else { continue }
      let decoded = SharedBoost.decode(fragment: fragment)
      // The JSON it holds, as a string or an object.
      var json: Data?
      if let string = entry["json"] as? String {
        json = Data(string.utf8)
      } else if let object = entry["json"] ?? entry["boost"], JSONSerialization.isValidJSONObject(object) {
        json = try JSONSerialization.data(withJSONObject: object)
      }
      if let json {
        #expect(decoded == SharedBoost.decode(json: json), "\(entry["name"] ?? fragment)")
      }
      if entry["error"] as? String == "refused" {
        // It decodes, but adding it breaks a save rule (match, name, size,
        // nothing to run): refused by resolveShared, as on the desktop.
        let shared = try #require(try? decoded.get(), "\(entry["name"] ?? fragment)")
        #expect((try? BoostList().resolveShared(shared).get()) == nil, "\(entry["name"] ?? fragment)")
      } else if entry["error"] != nil || entry["valid"] as? Bool == false {
        #expect((try? decoded.get()) == nil, "\(entry["name"] ?? fragment)")
      } else if json != nil {
        #expect((try? decoded.get()) != nil, "\(entry["name"] ?? fragment)")
      }
      checked += 1
    }
    #expect(checked > 0, "no vectors with a fragment")
  }

  /// A vector's fragment, or a whole link.
  private static func stripPrefix(_ text: String) -> String {
    guard let hash = text.firstIndex(of: "#") else { return text }
    return String(text[text.index(after: hash)...])
  }
}
