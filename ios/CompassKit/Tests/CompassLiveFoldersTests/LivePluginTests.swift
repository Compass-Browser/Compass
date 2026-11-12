// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassLiveFolders
import Foundation
import Testing

struct LivePluginTests {
  let scripts = try! LiveScripts(directory: LiveCaseTests.plugins)

  @Test func hashMatchesTheDesktop() {
    // Independently computed: sha256 over (8-byte LE length + bytes) of
    // each file.
    let hash = LivePluginPackage.hash(
      manifest: Data(#"{"id":"com.example.feed"}"#.utf8), entry: Data("async function fetchItems(){return {items:[]}}".utf8),
      icon: Data())
    #expect(hash == "e39a5ccee342614a659afb7e0748691a776588cccd3b045e10cfdca5e4beca66")
    #expect(LivePluginPackage.hash(manifest: Data("ab".utf8), entry: Data("c".utf8), icon: Data())
      != LivePluginPackage.hash(manifest: Data("a".utf8), entry: Data("bc".utf8), icon: Data()))
  }

  @Test func builtinsLoad() {
    let ids = scripts.builtins().map(\.id)
    #expect(ids == ["compass.calendar", "compass.github", "compass.rss", "compass.web-list"])
    let github = scripts.builtins().first { $0.id == "compass.github" }!
    #expect(github.manifest.secrets.map(\.key) == ["token"] && github.icon != nil)
    #expect(github.manifest.refresh.min >= 300)
  }

  @Test func manifestErrorsAreBadManifest() {
    let rules = LiveRules(scripts: scripts)
    #expect(throws: LiveError.self) { try rules.manifest(Data(#"{"id":"x"}"#.utf8)) }
    do {
      _ = try rules.manifest(Data("not json".utf8))
    } catch let error as LiveError {
      #expect(error.code == "bad_manifest")
    } catch {
      Issue.record("\(error)")
    }
  }

  @Test func packagesInstallAndReload() throws {
    let rules = LiveRules(scripts: scripts)
    let manifest = Data("""
      {"id": "com.example.feed", "name": "Feed", "version": "1.0", "api": 1, "hosts": ["example.com"]}
      """.utf8)
    let entry = Data("async function fetchItems() { return {items: []}; }".utf8)
    let source = URL(string: "https://example.com/plugin/manifest.json")!
    let package = try LivePluginPackage(source: source, manifestJSON: manifest, entry: entry, icon: Data(), rules: rules)
    #expect(package.sha256 == LivePluginPackage.hash(manifest: manifest, entry: entry, icon: Data()))

    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? FileManager.default.removeItem(at: dir) }
    _ = try package.install(in: dir)
    let installed = LivePluginPackage.installed(in: dir, rules: rules)
    #expect(installed.map(\.id) == ["com.example.feed"])
    #expect(installed.first?.sha256 == package.sha256 && installed.first?.sourceURL == source.absoluteString)

    // A built-in's ID can't be claimed.
    let claim = Data(#"{"id": "compass.rss", "name": "X", "version": "1", "api": 1}"#.utf8)
    #expect(throws: LiveError.self) {
      try LivePluginPackage(source: source, manifestJSON: claim, entry: entry, icon: Data(), rules: rules)
    }
    #expect(LivePluginPackage.isAllowedSource(URL(string: "http://localhost:8000/m.json")!))
    #expect(!LivePluginPackage.isAllowedSource(URL(string: "http://example.com/m.json")!))
  }

  @Test func pluginsCopiedInByHand() throws {
    let rules = LiveRules(scripts: scripts)
    let fm = FileManager.default
    let dir = fm.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    defer { try? fm.removeItem(at: dir) }
    let manifest = Data("""
      {"id": "com.example.hand", "name": "By Hand", "version": "1.0", "api": 1, "hosts": ["example.com"]}
      """.utf8)
    let entryName = try rules.manifest(manifest).entry
    let good = dir.appendingPathComponent("By Hand", isDirectory: true)
    try fm.createDirectory(at: good, withIntermediateDirectories: true)
    try manifest.write(to: good.appendingPathComponent("manifest.json"))
    try Data("async function fetchItems() { return {items: []}; }".utf8)
      .write(to: good.appendingPathComponent(entryName))
    // No manifest; a script missing; a built-in's ID; a loose file.
    try fm.createDirectory(at: dir.appendingPathComponent("Empty"), withIntermediateDirectories: true)
    let noScript = dir.appendingPathComponent("No Script", isDirectory: true)
    try fm.createDirectory(at: noScript, withIntermediateDirectories: true)
    try manifest.write(to: noScript.appendingPathComponent("manifest.json"))
    let claim = dir.appendingPathComponent("Claim", isDirectory: true)
    try fm.createDirectory(at: claim, withIntermediateDirectories: true)
    try Data(#"{"id": "compass.rss", "name": "X", "version": "1", "api": 1}"#.utf8)
      .write(to: claim.appendingPathComponent("manifest.json"))
    try Data("x".utf8).write(to: claim.appendingPathComponent(entryName))
    try Data("note".utf8).write(to: dir.appendingPathComponent("Read Me.txt"))

    let found = LivePluginPackage.manual(in: dir, rules: rules)
    #expect(found.plugins.map(\.id) == ["com.example.hand"])
    #expect(found.plugins.first?.sourceURL.hasPrefix("file:") == true)
    #expect(found.problems.count == 3)
    #expect(found.problems.contains { $0.hasPrefix("Empty: ") && $0.contains("manifest.json") })
    #expect(found.problems.contains { $0.hasPrefix("No Script: ") })
    #expect(found.problems.contains { $0.hasPrefix("Claim: ") })
  }

  @Test func redirectsAndCookieHosts() {
    let allowed = LiveAllowedHosts(hosts: ["example.com", "*.cdn.example.net"], cookies: ["example.com"])
    #expect(allowed.allows(URL(string: "https://example.com/a")!))
    #expect(allowed.allows(URL(string: "https://x.cdn.example.net/a")!))
    #expect(!allowed.allows(URL(string: "https://evil.example.org/")!))
    #expect(!allowed.allows(URL(string: "ftp://example.com/")!))
    #expect(!allowed.allows(URL(string: "https://u:p@example.com/")!))
    #expect(allowed.allowsCookies(URL(string: "https://EXAMPLE.com./")!))
    #expect(!allowed.allowsCookies(URL(string: "https://x.cdn.example.net/")!))
  }
}

/// The network path (URLSessionLiveFetcher) against real sites; opt in
/// with COMPASS_LIVE_NETWORK=1.
struct LiveNetworkTests {
  static let enabled = ProcessInfo.processInfo.environment["COMPASS_LIVE_NETWORK"] == "1"

  @Test(.enabled(if: enabled)) func feedOverTheNetwork() async throws {
    let scripts = try LiveScripts(directory: LiveCaseTests.plugins)
    let rss = scripts.builtins().first { $0.id == "compass.rss" }!
    // http → https redirect on the same host, checked against the hosts.
    let run = await LiveRuntime(scripts: scripts).run(
      rss, configJSON: #"{"url": "http://www.apple.com/newsroom/rss-feed.rss"}"#, fetcher: URLSessionLiveFetcher())
    let result = try run.result.get()
    #expect(!result.items.isEmpty)
    #expect(run.fetches.count == 1)
  }

  @Test(.enabled(if: enabled)) func redirectOffTheAllowlistFails() async throws {
    let fetcher = URLSessionLiveFetcher()
    let request = LiveFetchRequest(
      method: "GET", url: URL(string: "http://github.com/")!, headers: [:], body: nil, withCookies: false)
    do {
      _ = try await fetcher.fetch(request, allowed: LiveAllowedHosts(hosts: ["github.com"], cookies: []), limits: LiveLimits())
    } catch let error as LiveError {
      Issue.record("same-host redirect refused: \(error)")
    }
    var limits = LiveLimits()
    limits.maxResponseBytes = 1000
    await #expect(throws: LiveError("response_too_large", "Response from github.com is too large")) {
      _ = try await fetcher.fetch(
        LiveFetchRequest(method: "GET", url: URL(string: "https://github.com/")!, headers: [:], body: nil, withCookies: false),
        allowed: LiveAllowedHosts(hosts: ["github.com"], cookies: []), limits: limits)
    }
    let off = LiveFetchRequest(
      method: "GET", url: URL(string: "https://apple.com/")!, headers: [:], body: nil, withCookies: false)
    do {
      _ = try await fetcher.fetch(off, allowed: LiveAllowedHosts(hosts: ["apple.com"], cookies: []), limits: LiveLimits())
      Issue.record("redirect to www.apple.com was allowed")
    } catch let error as LiveError {
      #expect(error.code == "host_not_allowed")
    }
  }
}
