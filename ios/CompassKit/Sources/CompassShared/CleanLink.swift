// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Copy Link without the query parameters that only track where a link was
/// shared or clicked, as Firefox's "Copy Clean Link" removes them
/// (docs/design/ios.md, "Clean copied links"). The desktop's
/// common/clean_url.cc, ported: the same lists
/// (third_party/strip_on_share/, bundled unmodified) and the same rules.
public struct CleanLink: Sendable {
  /// The setting, on unless turned off: the desktop's pref name.
  public static let removeTrackingKey = "compass.copy.remove_tracking"

  public static func isOn(_ defaults: UserDefaults = .standard) -> Bool {
    defaults.object(forKey: removeTrackingKey) as? Bool ?? true
  }

  /// Firefox's two lists, as the app bundles them: third_party/strip_on_share/
  /// as a folder, so its paths stay (and its two LICENSE files apart).
  public static let listFiles = ["strip_on_share/MPL2/StripOnShare.json", "strip_on_share/LGPL/StripOnShareLGPL.json"]

  /// Names in lower case.
  private var global: Set<String> = []
  private var byHost: [String: Set<String>] = [:]
  private var bySite: [String: Set<String>] = [:]

  /// Both lists merged; rules of the same name are combined. Throws if one
  /// isn't a JSON object.
  public init(lists: [Data]) throws {
    for data in lists {
      guard let list = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
        throw CocoaError(.propertyListReadCorrupt)
      }
      for case let rule as [String: Any] in list.values {
        let names = Set(((rule["queryParams"] as? [Any]) ?? []).compactMap { ($0 as? String).map(Self.lower) })
        if rule["isGlobal"] as? Bool == true { global.formUnion(names) }
        for case let host as String in (rule["hosts"] as? [Any]) ?? [] {
          byHost[Self.lower(host), default: []].formUnion(names)
        }
        for case let site as String in (rule["schemelessSites"] as? [Any]) ?? [] {
          bySite[Self.lower(site), default: []].formUnion(names)
        }
      }
    }
  }

  /// The lists from `bundle` (the app's), or nil if they're missing.
  public init?(bundle: Bundle) {
    let lists = Self.listFiles.compactMap { path in
      bundle.resourceURL.flatMap { try? Data(contentsOf: $0.appendingPathComponent(path)) }
    }
    guard lists.count == Self.listFiles.count, let rules = try? Self(lists: lists) else { return nil }
    self = rules
  }

  /// `url` without tracking parameters: global ones and those of its host
  /// or site. Also cleans an http(s) URL held in a parameter's value
  /// (?url=https://…), one level deep. Every other parameter keeps its
  /// order and spelling; a query left empty goes, "?" included. The
  /// fragment stays. URLs other than http and https come back unchanged.
  public func clean(_ url: URL) -> URL {
    guard let cleaned = clean(url.absoluteString, nested: true) else { return url }
    return URL(string: cleaned) ?? url
  }

  /// The string form; nil when nothing was removed.
  func clean(_ link: String, nested: Bool) -> String? {
    guard let url = URL(string: link), let scheme = url.scheme.map(Self.lower), scheme == "http" || scheme == "https"
    else { return nil }
    // The query, raw: from the first "?" to the fragment's "#".
    let beforeFragment = link.firstIndex(of: "#") ?? link.endIndex
    guard let mark = link[..<beforeFragment].firstIndex(of: "?") else { return nil }
    let query = link[link.index(after: mark)..<beforeFragment]
    if query.isEmpty { return nil }

    let host = Self.lower(url.host(percentEncoded: false) ?? "")
    let hostNames = byHost[host]
    let siteNames = siteNames(host)

    var kept: [String] = []
    var removed = false
    for part in query.split(separator: "&", omittingEmptySubsequences: false) {
      let equals = part.firstIndex(of: "=")
      // Compared as written, not unescaped: the lists spell out the escaped
      // names they want ("__cft__%5B0%5D").
      let name = Self.lower(String(part[..<(equals ?? part.endIndex)]))
      if !name.isEmpty
        && (global.contains(name) || hostNames?.contains(name) == true || siteNames.contains(name))
      {
        removed = true
        continue
      }
      if nested, let equals,
        let value = String(part[part.index(after: equals)...]).replacingOccurrences(of: "+", with: " ")
          .removingPercentEncoding,
        let inner = clean(value, nested: false)
      {
        removed = true
        kept.append(String(part[...equals]) + Self.escapeQueryValue(inner))
        continue
      }
      kept.append(String(part))
    }
    guard removed else { return nil }
    let newQuery = kept.joined(separator: "&")
    return String(link[..<mark]) + (newQuery.isEmpty ? "" : "?" + newQuery) + String(link[beforeFragment...])
  }

  /// The names for `host`'s site. The desktop asks for the registrable
  /// domain (eTLD+1, from the public suffix list); iOS has no public suffix
  /// list, and every `schemelessSites` entry in Firefox's lists is a
  /// registrable domain, so a host matches a site it equals or ends with
  /// as ".site". The same answers for those entries, IP addresses included.
  private func siteNames(_ host: String) -> Set<String> {
    var names: Set<String> = []
    var rest = Substring(host)
    while !rest.isEmpty {
      if let found = bySite[String(rest)] { names.formUnion(found) }
      guard let dot = rest.firstIndex(of: ".") else { break }
      rest = rest[rest.index(after: dot)...]
    }
    return names
  }

  private static func lower(_ text: String) -> String {
    String(String.UnicodeScalarView(text.unicodeScalars.map { ("A"..."Z").contains($0) ? Unicode.Scalar($0.value + 32)! : $0 }))
  }

  /// Chromium's EscapeQueryParamValue(use_plus: true): %XX everything but
  /// alphanumerics and -_.!~*'(), spaces as "+".
  static func escapeQueryValue(_ text: String) -> String {
    var out = ""
    for byte in text.utf8 {
      switch byte {
      case UInt8(ascii: "a")...UInt8(ascii: "z"), UInt8(ascii: "A")...UInt8(ascii: "Z"),
        UInt8(ascii: "0")...UInt8(ascii: "9"):
        out.unicodeScalars.append(Unicode.Scalar(byte))
      case _ where "-_.!~*'()".utf8.contains(byte): out.unicodeScalars.append(Unicode.Scalar(byte))
      case UInt8(ascii: " "): out += "+"
      default: out += String(format: "%%%02X", byte)
      }
    }
    return out
  }
}
