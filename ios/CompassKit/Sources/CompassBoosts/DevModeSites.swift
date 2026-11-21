// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// The sites in development mode (docs/design/dev-mode.md, "iOS"): host
/// patterns as the desktop keeps them ("localhost", "example.com",
/// "*.example.com"), matched as Boosts' (BoostMatch, without a path).
public struct DevModeSites: Equatable, Sendable {
  /// In the order they were added.
  public private(set) var patterns: [String]

  public init(_ patterns: [String] = []) {
    self.patterns = patterns.filter { BoostMatch($0) != nil }
  }

  /// The pattern to keep for what was typed or a page's host, or nil for
  /// what isn't a web host. As the desktop's NormalizeHost(): a scheme, a
  /// port and a path are dropped, "www." is the site itself, and a leading
  /// "*." keeps the subdomains.
  public static func normalize(_ input: String) -> String? {
    var text = input.trimmingCharacters(in: .whitespacesAndNewlines)
    var subdomains = false
    if text.hasPrefix("*.") {
      subdomains = true
      text.removeFirst(2)
    }
    guard !text.isEmpty else { return nil }
    if !text.contains("://") { text = "http://" + text }
    guard let url = URL(string: text), let scheme = url.scheme?.lowercased(),
          scheme == "http" || scheme == "https",
          var host = url.host(percentEncoded: false)?.lowercased(), !host.isEmpty
    else { return nil }
    if host.hasSuffix(".") { host.removeLast() }
    if !subdomains && host.hasPrefix("www.") { host.removeFirst(4) }
    // "*." over an IP address isn't a domain.
    if subdomains && host.allSatisfy({ $0.isNumber || $0 == "." }) { return nil }
    let pattern = (subdomains ? "*." : "") + host
    guard let match = BoostMatch(pattern), match.subdomains == subdomains,
          !subdomains || host.contains(".")
    else { return nil }
    return match.pattern
  }

  /// Whether a page at `url` is in development mode: http and https.
  public func contains(_ url: URL?) -> Bool {
    guard let url else { return false }
    return patterns.contains { BoostMatch($0)?.matches(url) == true }
  }

  /// Adds `input` as normalize() keeps it, unless it's there. The pattern,
  /// or nil when it isn't a host.
  @discardableResult
  public mutating func add(_ input: String) -> String? {
    guard let pattern = Self.normalize(input) else { return nil }
    if !patterns.contains(pattern) { patterns.append(pattern) }
    return pattern
  }

  public mutating func remove(_ pattern: String) {
    patterns.removeAll { $0 == pattern }
  }

  /// On: adds `url`'s host. Off: removes every pattern that matches `url`,
  /// so it's off there even when "*.example.com" put it on.
  public mutating func set(_ url: URL, enabled: Bool) {
    guard let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https",
          let host = url.host(percentEncoded: false)
    else { return }
    if enabled {
      if !contains(url) { add(host) }
    } else {
      patterns.removeAll { BoostMatch($0)?.matches(url) == true }
    }
  }
}
