// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassLiveFolders
import Foundation
import Testing

/// The fixture cases every runtime passes (docs/design/live-folders.md,
/// "Testing"): plugins/builtin/*/tests/* and plugins/conformance/*, as
/// tools/live_folders/run.mjs and the desktop's
/// LiveFolderRuntimeBrowserTest run them.
struct LiveCaseTests {
  static let repo = URL(fileURLWithPath: #filePath)
    .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    .deletingLastPathComponent().deletingLastPathComponent()
  static let plugins = repo.appendingPathComponent("plugins")

  struct Case: CustomTestStringConvertible, Sendable {
    let name: String
    let dir: URL
    let pluginDir: URL
    var testDescription: String { name }
  }

  static let cases: [Case] = {
    let fm = FileManager.default
    var out: [Case] = []
    let builtin = plugins.appendingPathComponent("builtin")
    for name in ((try? fm.contentsOfDirectory(atPath: builtin.path)) ?? []).sorted() {
      let tests = builtin.appendingPathComponent(name).appendingPathComponent("tests")
      for c in ((try? fm.contentsOfDirectory(atPath: tests.path)) ?? []).sorted() {
        let dir = tests.appendingPathComponent(c)
        if fm.fileExists(atPath: dir.appendingPathComponent("case.json").path) {
          out.append(Case(name: "\(name)/\(c)", dir: dir, pluginDir: builtin.appendingPathComponent(name)))
        }
      }
    }
    let conformance = plugins.appendingPathComponent("conformance")
    for c in ((try? fm.contentsOfDirectory(atPath: conformance.path)) ?? []).sorted() {
      let dir = conformance.appendingPathComponent(c)
      if fm.fileExists(atPath: dir.appendingPathComponent("case.json").path) {
        out.append(Case(name: "conformance/\(c)", dir: dir, pluginDir: dir.appendingPathComponent("plugin")))
      }
    }
    return out
  }()

  /// Recorded responses: {"<url>" | "POST <url>": {status, headers, file |
  /// body | json, rejectHeaders}}.
  struct FixtureFetcher: LiveFetcher, @unchecked Sendable {
    let responses: [String: Any]
    let dir: URL

    init(responses: [String: Any], dir: URL) {
      self.responses = responses
      self.dir = dir
    }

    func fetch(_ request: LiveFetchRequest, allowed: LiveAllowedHosts, limits: LiveLimits) async throws
      -> LiveFetchResponse
    {
      let url = request.url.absoluteString
      guard
        let entry = (responses["\(request.method) \(url)"] ?? (request.method == "GET" ? responses[url] : nil))
          as? [String: Any]
      else { throw LiveError("network", "No fixture for \(request.method) \(url)") }
      for header in entry["rejectHeaders"] as? [String] ?? [] where request.headers.keys.contains(header) {
        throw LiveError("network", "Fixture \(url) received header \(header)")
      }
      let body: Data
      if let file = entry["file"] as? String {
        body = try Data(contentsOf: dir.appendingPathComponent(file))
      } else if let json = entry["json"] {
        body = try JSONSerialization.data(withJSONObject: json, options: [.fragmentsAllowed, .withoutEscapingSlashes])
      } else {
        body = Data((entry["body"] as? String ?? "").utf8)
      }
      return LiveFetchResponse(
        status: entry["status"] as? Int ?? 200, url: request.url, headers: entry["headers"] as? [String: String] ?? [:],
        body: body)
    }
  }

  struct Expected: Decodable, Equatable {
    struct Code: Decodable, Equatable { var code: String }
    struct Fetch: Decodable, Equatable {
      var method: String
      var url: String
      var cookies: Bool
    }
    var ok: Bool
    var result: LiveResult?
    var error: Code?
    var fetches: [Fetch]
  }

  @Test(arguments: cases)
  func fixture(_ c: Case) async throws {
    let scripts = try LiveScripts(directory: Self.plugins)
    let spec = try JSONSerialization.jsonObject(with: Data(contentsOf: c.dir.appendingPathComponent("case.json")))
      as? [String: Any] ?? [:]
    let expected = try JSONDecoder().decode(
      Expected.self, from: Data(contentsOf: c.dir.appendingPathComponent("expected.json")))

    var actual: Expected
    do {
      let plugin = try LiveRules(scripts: scripts).loadPlugin(at: c.pluginDir)
      let env = spec["env"] as? [String: Any] ?? [:]
      var now = Date()
      if let s = env["now"] as? String { now = try Date(s, strategy: .iso8601) }
      if let n = env["now"] as? Double { now = Date(timeIntervalSince1970: n / 1000) }
      var limits = LiveLimits()
      let given = spec["limits"] as? [String: Any] ?? [:]
      if let ms = given["timeoutMs"] as? Double { limits.timeout = ms / 1000 }
      if let n = given["maxFetches"] as? Int { limits.maxFetches = n }
      if let n = given["maxResponseBytes"] as? Int { limits.maxResponseBytes = n }
      if let n = given["maxRedirects"] as? Int { limits.maxRedirects = n }
      if let n = given["maxItems"] as? Int { limits.maxItems = n }
      let config = try JSONSerialization.data(withJSONObject: spec["config"] ?? [String: Any]())
      let run = await LiveRuntime(scripts: scripts).run(
        plugin, configJSON: String(decoding: config, as: UTF8.self),
        secrets: spec["secrets"] as? [String: String] ?? [:], grantCookies: spec["grantCookies"] as? Bool ?? false,
        environment: LiveEnvironment(
          now: now, locale: env["locale"] as? String ?? "en-US", timeZone: env["timeZone"] as? String ?? "UTC"),
        limits: limits, fetcher: FixtureFetcher(responses: spec["responses"] as? [String: Any] ?? [:], dir: c.dir))
      let fetches = run.fetches.map { Expected.Fetch(method: $0.method, url: $0.url, cookies: $0.cookies) }
      switch run.result {
      case .success(let result): actual = Expected(ok: true, result: result, error: nil, fetches: fetches)
      case .failure(let error):
        actual = Expected(ok: false, result: nil, error: .init(code: error.code), fetches: fetches)
        if error.code != expected.error?.code { Issue.record("\(c.name): \(error.code): \(error.message)") }
      }
    } catch let error as LiveError {
      actual = Expected(ok: false, result: nil, error: .init(code: error.code), fetches: [])
    }
    #expect(actual == expected, "\(c.name)")
  }

  @Test func casesFound() {
    #expect(Self.cases.count >= 27)
  }
}
