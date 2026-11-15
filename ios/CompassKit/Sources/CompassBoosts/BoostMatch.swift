// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Where a Boost applies (docs/design/boosts.md), the desktop's match
/// pattern:
/// - "example.com": that host and "www.example.com";
/// - "*.example.com": the domain and every subdomain of it;
/// - either one followed by a path prefix: "github.com/settings" applies
///   to the pages whose path starts with "/settings".
/// Only http and https pages; the port doesn't matter.
public struct BoostMatch: Equatable, Hashable, Sendable {
  /// Lowercase, no trailing dot, no "*.".
  public let host: String
  /// "*.": the domain and its subdomains.
  public let subdomains: Bool
  /// "" for every page, else a prefix starting with "/".
  public let path: String

  /// The pattern as stored: "*.example.com/path".
  public var pattern: String { (subdomains ? "*." : "") + host + path }

  /// The host a person reads: "example.com" for "*.example.com".
  public var site: String { host }

  /// nil when `pattern` isn't one. Lenient on input: surrounding spaces, an
  /// "http(s)://" prefix, uppercase, a trailing dot and a trailing "*" on
  /// the path are fixed up.
  public init?(_ pattern: String) {
    var text = pattern.trimmingCharacters(in: .whitespacesAndNewlines)
    for scheme in ["https://", "http://"] where text.lowercased().hasPrefix(scheme) {
      text = String(text.dropFirst(scheme.count))
    }
    var hostPart = text
    var path = ""
    if let slash = text.firstIndex(of: "/") {
      hostPart = String(text[..<slash])
      path = String(text[slash...])
    }
    if path.hasSuffix("*") { path.removeLast() }
    if path == "/" { path = "" }
    guard !path.contains(where: { $0.isWhitespace || $0 == "?" || $0 == "#" || $0 == "*" }) else { return nil }

    var host = hostPart.lowercased()
    var subdomains = false
    if host.hasPrefix("*.") {
      subdomains = true
      host.removeFirst(2)
    }
    if host.hasSuffix(".") { host.removeLast() }
    guard Self.isValidHost(host) else { return nil }
    self.host = host
    self.subdomains = subdomains
    self.path = path
  }

  /// ASCII letters, digits, "-" and "." (IDN as punycode), no empty label.
  static func isValidHost(_ host: String) -> Bool {
    guard !host.isEmpty, host.utf8.count <= 253 else { return false }
    let allowed = Set("abcdefghijklmnopqrstuvwxyz0123456789-.")
    guard host.allSatisfy(allowed.contains) else { return false }
    return !host.split(separator: ".", omittingEmptySubsequences: false).contains { $0.isEmpty }
  }

  /// Whether a page at `url` gets the Boost.
  public func matches(_ url: URL) -> Bool {
    guard let scheme = url.scheme?.lowercased(), let host = url.host else { return false }
    var path = url.path(percentEncoded: true)
    if path.isEmpty { path = "/" }
    return matches(scheme: scheme, host: host, path: path)
  }

  /// The same from a URL's parts: `path` as the page has it (percent
  /// encoded, as `location.pathname`).
  public func matches(scheme: String, host: String, path: String) -> Bool {
    guard scheme == "http" || scheme == "https" else { return false }
    return matchesHost(host) && (self.path.isEmpty || path.hasPrefix(self.path))
  }

  /// Whether pages of `host` get the Boost (on some path).
  public func matchesHost(_ host: String) -> Bool {
    var host = host.lowercased()
    if host.hasSuffix(".") { host.removeLast() }
    if host == self.host { return true }
    return subdomains ? host.hasSuffix("." + self.host) : host == "www." + self.host
  }

  /// A JavaScript expression, true in a page (or frame) the Boost applies
  /// to: WKUserScript has no URL matching of its own, so each Boost's
  /// script checks `location` first. The same rules as `matches`.
  public var javaScriptCondition: String {
    let host = BoostScripts.literal(self.host)
    let hostTest = subdomains ? "h.endsWith('.'+\(host))" : "h==='www.'+\(host)"
    let pathTest = path.isEmpty ? "true" : "l.pathname.indexOf(\(BoostScripts.literal(path)))===0"
    return
      "(function(){var l=location;if(l.protocol!=='http:'&&l.protocol!=='https:')return false;var h=l.hostname.toLowerCase().replace(/\\.$/,'');return (h===\(host)||\(hostTest))&&\(pathTest);})()"
  }

  /// "Boost this site": the pattern for a page's host, without "www.".
  public static func suggested(for url: URL) -> String? {
    guard let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https",
      var host = url.host?.lowercased(), !host.isEmpty
    else { return nil }
    if host.hasSuffix(".") { host.removeLast() }
    if host.hasPrefix("www."), host.count > 4 { host.removeFirst(4) }
    return BoostMatch(host)?.pattern
  }
}
