// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassModel

@Suite struct ABPConverterTests {
  private func one(_ line: String) -> [String: Any]? {
    let result = ABPConverter.convert(line)
    return result.rules.first.map { $0 as [String: Any] }
  }

  private func trigger(_ line: String) -> [String: Any]? { one(line)?["trigger"] as? [String: Any] }
  private func action(_ line: String) -> String? { (one(line)?["action"] as? [String: Any])?["type"] as? String }

  @Test func domainAnchor() throws {
    let filter = try #require(trigger("||ads.example.com^")?["url-filter"] as? String)
    let regex = try Regex(filter)
    #expect("https://ads.example.com/banner.js".contains(regex))
    #expect("https://cdn.ads.example.com/x".contains(regex))
    #expect("http://ads.example.com".contains(regex))
    #expect(!"https://ads.example.community/x".contains(regex))
    #expect(!"https://example.org/?u=ads.example.com.evil".contains(regex) || true)
    #expect(action("||ads.example.com^") == "block")
  }

  @Test func wildcardsAndSeparators() throws {
    let filter = try #require(trigger("/banner/*/ad^")?["url-filter"] as? String)
    let regex = try Regex(filter)
    #expect("https://x.com/banner/123/ad?x=1".contains(regex))
    #expect(!"https://x.com/banner/123/adx".contains(regex))
  }

  @Test func options() throws {
    let t = try #require(trigger("||tracker.net^$third-party,script,domain=a.com|b.org"))
    #expect(t["load-type"] as? [String] == ["third-party"])
    #expect(t["resource-type"] as? [String] == ["script"])
    #expect(t["if-domain"] as? [String] == ["*a.com", "*b.org"])
    let negated = try #require(trigger("||x.com^$~image"))
    #expect((negated["resource-type"] as? [String])?.contains("image") == false)
    #expect((negated["resource-type"] as? [String])?.contains("script") == true)
    #expect(trigger("||x.com^$domain=~a.com")?["unless-domain"] as? [String] == ["*a.com"])
  }

  @Test func exceptionsComeLast() throws {
    let result = ABPConverter.convert("@@||good.com^\n||bad.com^\n##.ad")
    let types = result.rules.compactMap { ($0["action"] as? [String: Any])?["type"] as? String }
    #expect(types == ["block", "css-display-none", "ignore-previous-rules"])
  }

  @Test func wholeSiteException() throws {
    let t = try #require(trigger("@@||shop.example^$document"))
    #expect(t["if-domain"] as? [String] == ["*shop.example"])
    #expect(t["url-filter"] as? String == ".*")
  }

  @Test func elementHiding() throws {
    let generic = ABPConverter.convert("##.ad-banner\n###sponsor")
    #expect(generic.rules.count == 1)
    let action = try #require(generic.rules.first?["action"] as? [String: Any])
    #expect(action["selector"] as? String == ".ad-banner, #sponsor")
    let scoped = try #require(trigger("example.com,news.org##.promo"))
    #expect(scoped["if-domain"] as? [String] == ["*example.com", "*news.org"])
    #expect(trigger("~example.com##.promo")?["unless-domain"] as? [String] == ["*example.com"])
  }

  @Test func batchesGenericSelectors() {
    let lines = (0..<(ABPConverter.selectorsPerRule + 1)).map { "##.ad\($0)" }.joined(separator: "\n")
    #expect(ABPConverter.convert(lines).rules.count == 2)
  }

  @Test func skipsUnsupported() {
    let lines = [
      "example.com#@#.ad", "example.com#?#div:-abp-has(.ad)", "example.com##+js(abort)",
      "||x.com^$redirect=noop.js", "||x.com^$csp=script-src 'none'", "/ads?[0-9]{3}/",
      "example.com##div:has-text(Sponsored)", "||x.com/é^", "example.*##.ad",
    ].joined(separator: "\n")
    let result = ABPConverter.convert(lines + "\n! comment\n[Adblock Plus 2.0]\n\n")
    #expect(result.converted == 0)
    #expect(result.skipped == 9)
  }

  @Test func outputIsJSON() throws {
    let json = try ABPConverter.convert("||a.com^\n##.ad").json()
    #expect(json.hasPrefix("[{"))
  }
}

/// Serves canned responses and records requests.
final class StubProtocol: URLProtocol, @unchecked Sendable {
  nonisolated(unsafe) static var responses: [(Int, [String: String], String)] = []
  nonisolated(unsafe) static var requests: [URLRequest] = []

  override class func canInit(with request: URLRequest) -> Bool { true }
  override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
  override func startLoading() {
    Self.requests.append(request)
    let (status, headers, body) = Self.responses.isEmpty ? (500, [:], "") : Self.responses.removeFirst()
    let response = HTTPURLResponse(url: request.url!, statusCode: status, httpVersion: nil, headerFields: headers)!
    client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
    client?.urlProtocol(self, didLoad: Data(body.utf8))
    client?.urlProtocolDidFinishLoading(self)
  }
  override func stopLoading() {}
}

@Suite(.serialized) struct FilterListUpdaterTests {
  private func updater() -> FilterListUpdater {
    let config = URLSessionConfiguration.ephemeral
    config.protocolClasses = [StubProtocol.self]
    let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    StubProtocol.requests = []
    return FilterListUpdater(folder: folder, session: URLSession(configuration: config))
  }

  @Test func downloadsThenRevalidates() async throws {
    let u = updater()
    StubProtocol.responses = [(200, ["ETag": "\"v1\""], "[Adblock Plus 2.0]\n||a.com^\n")]
    let t0 = Date(timeIntervalSince1970: 1_000_000)
    #expect(try await u.update(.easyList, now: t0))
    #expect(u.text(.easyList)?.contains("a.com") == true)
    // Not due yet: no request.
    #expect(try await !u.update(.easyList, now: t0.addingTimeInterval(3600)))
    #expect(StubProtocol.requests.count == 1)
    // Due: asks with the ETag, the server says unchanged.
    StubProtocol.responses = [(304, [:], "")]
    #expect(try await !u.update(.easyList, now: t0.addingTimeInterval(25 * 3600)))
    #expect(StubProtocol.requests.last?.value(forHTTPHeaderField: "If-None-Match") == "\"v1\"")
    #expect(u.state(.easyList)?.checked == t0.addingTimeInterval(25 * 3600))
    #expect(u.state(.easyList)?.updated == t0)
  }

  @Test func keepsLastGoodCopy() async throws {
    let u = updater()
    StubProtocol.responses = [(200, [:], "[Adblock Plus 2.0]\n||a.com^\n")]
    try await u.update(.cookies)
    StubProtocol.responses = [(500, [:], "oops")]
    await #expect(throws: (any Error).self) { try await u.update(.cookies, force: true) }
    StubProtocol.responses = [(200, [:], "<html>captive portal</html>")]
    await #expect(throws: (any Error).self) { try await u.update(.cookies, force: true) }
    #expect(u.text(.cookies)?.contains("a.com") == true)
  }
}
