// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// A link rule (docs/design/link-routing.md): links from other apps on
/// `domain` or its subdomains open in the space `spaceID`. One per domain
/// in each profile; it outlives its space (then it routes nothing).
public struct LinkRule: Equatable, Hashable, Sendable, Identifiable {
  /// Normalized (`LinkRouting.normalizedDomain`): lowercase ASCII, IDN as
  /// punycode, no "www.".
  public var domain: String
  public var spaceID: String
  /// The profile's UUID, empty for the home profile.
  public var profile: String

  public struct ID: Hashable, Sendable {
    public var profile: String
    public var domain: String

    public init(profile: String, domain: String) {
      self.profile = profile
      self.domain = domain
    }
  }

  public var id: ID { ID(profile: profile, domain: domain) }

  public init(domain: String, spaceID: String, profile: String = "") {
    self.domain = domain
    self.spaceID = spaceID
    self.profile = profile
  }
}

/// Where a link from another app goes (`SpaceStore.opening`).
public enum LinkOpening: Equatable, Sendable {
  /// A new Today tab of this space (by a rule), shown.
  case space(String)
  /// A little window over the shown space (no rule).
  case littleWindow
  /// A new Today tab of the shown space (no rule, little windows off).
  case shownSpace
}

/// Domains and matching as the desktop's `link_rules.cc`: a rule's domain
/// covers its subdomains, and the most specific rule wins (the host, then
/// each parent domain).
public enum LinkRouting {
  /// The rule for `url`, if any: http(s) links on a domain name only (not
  /// an IP address). Of rules for the same domain, the first in `rules`.
  public static func rule(for url: URL, in rules: [LinkRule]) -> LinkRule? {
    guard let host = host(of: url) else { return nil }
    return rule(forHost: host, in: rules)
  }

  /// The most specific rule for a host (as `host(of:)` gives it).
  public static func rule(forHost host: String, in rules: [LinkRule]) -> LinkRule? {
    var byDomain: [String: LinkRule] = [:]
    for rule in rules where byDomain[rule.domain] == nil { byDomain[rule.domain] = rule }
    var rest = Substring(host)
    while !rest.isEmpty {
      if let rule = byDomain[String(rest)] { return rule }
      guard let dot = rest.firstIndex(of: ".") else { break }
      rest = rest[rest.index(after: dot)...]
    }
    return nil
  }

  /// Whether `host` is `domain` or one of its subdomains.
  public static func matches(host: String, domain: String) -> Bool {
    !domain.isEmpty && (host == domain || host.hasSuffix("." + domain))
  }

  /// A web link's host as rules see it (the desktop's GURL host):
  /// lowercase, IDN as punycode, no trailing dot; nil for other schemes,
  /// IP addresses and hosts that aren't names.
  public static func host(of url: URL) -> String? {
    guard let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https",
      let components = URLComponents(url: url, resolvingAgainstBaseURL: false),
      let raw = components.percentEncodedHost, !raw.isEmpty, !raw.hasPrefix("[")
    else { return nil }
    guard let host = canonicalHost(raw), !isIPAddress(host) else { return nil }
    return host
  }

  /// A rule's domain from what's typed, as the desktop's
  /// `NormalizeLinkRuleDomain()`: a domain ("GitHub.com", "*.github.com",
  /// "bücher.de") or a URL ("https://www.github.com/compass" gives
  /// "github.com"). Lowercase, IDN as punycode ("xn--bcher-kva.de"), without
  /// a leading "www." or "*.", a trailing dot, port or path. nil for what
  /// isn't one: empty text, spaces, other schemes, IP addresses, single
  /// names ("localhost") and public suffixes ("com", "co.uk").
  public static func normalizedDomain(_ text: String) -> String? {
    var s = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !s.isEmpty, !s.contains(where: \.isWhitespace) else { return nil }
    if let range = s.range(of: "://") {
      let scheme = s[..<range.lowerBound].lowercased()
      guard scheme == "http" || scheme == "https" else { return nil }
      s = String(s[range.upperBound...])
    } else if s.hasPrefix("*.") {
      s.removeFirst(2)
    }
    if let i = s.firstIndex(where: { "/?#\\".contains($0) }) { s = String(s[..<i]) }
    if let at = s.lastIndex(of: "@") { s = String(s[s.index(after: at)...]) }
    if s.hasPrefix("[") { return nil }  // IPv6
    if let colon = s.lastIndex(of: ":") {
      guard s[s.index(after: colon)...].allSatisfy(\.isASCII) else { return nil }
      s = String(s[..<colon])
    }
    guard !s.contains("*"), var domain = canonicalHost(s), !isIPAddress(domain) else { return nil }
    if domain.hasPrefix("www."), domain.dropFirst(4).contains(".") { domain.removeFirst(4) }
    guard isUnderARegistry(domain) else { return nil }
    return domain
  }

  /// A normalized domain for display: punycode labels back in Unicode
  /// ("xn--bcher-kva.de" → "bücher.de").
  public static func displayDomain(_ domain: String) -> String {
    domain.split(separator: ".").map { label -> String in
      guard label.hasPrefix("xn--"), let decoded = Punycode.decode(String(label.dropFirst(4))) else {
        return String(label)
      }
      return decoded
    }.joined(separator: ".")
  }

  /// The domain "Always Open <domain> in" offers for a page: its host,
  /// normalized (without "www.").
  public static func suggestedDomain(for url: URL) -> String? {
    host(of: url).flatMap(normalizedDomain)
  }

  /// A host as GURL canonicalizes it: percent-decoded, lowercase, IDN
  /// labels as punycode, one trailing dot dropped. nil with an empty label
  /// (a leading dot, "..") or a character a host can't have.
  static func canonicalHost(_ text: String) -> String? {
    var s = text.removingPercentEncoding ?? text
    // The ideographic and full-width full stops are dots too (IDNA).
    for dot in ["\u{3002}", "\u{FF0E}", "\u{FF61}"] { s = s.replacingOccurrences(of: dot, with: ".") }
    s = s.precomposedStringWithCompatibilityMapping.lowercased()
    if s.hasSuffix(".") { s.removeLast() }
    guard !s.isEmpty else { return nil }
    var labels: [String] = []
    for label in s.split(separator: ".", omittingEmptySubsequences: false) {
      guard !label.isEmpty else { return nil }
      if label.unicodeScalars.allSatisfy(\.isASCII) {
        guard label.unicodeScalars.allSatisfy(isHostCharacter) else { return nil }
        labels.append(String(label))
      } else {
        guard let encoded = Punycode.encode(String(label)) else { return nil }
        labels.append("xn--" + encoded)
      }
      guard labels.last!.utf8.count <= 63 else { return nil }
    }
    let host = labels.joined(separator: ".")
    return host.utf8.count <= 253 ? host : nil
  }

  /// A host GURL would read as an IPv4 address: its last label is a number
  /// ("10.0.0.1", "0x7f.1").
  static func isIPAddress(_ host: String) -> Bool {
    guard let last = host.split(separator: ".").last else { return false }
    if last.allSatisfy(\.isNumber) { return true }
    return last.hasPrefix("0x") && last.dropFirst(2).allSatisfy(\.isHexDigit)
  }

  /// Whether a domain can be registered or is under one: not a single name
  /// ("com", "localhost") or a public suffix ("co.uk"). The desktop asks
  /// the public suffix list; this approximates it with the common
  /// second-level registries of country domains ("co.uk", "com.au",
  /// "ne.jp"). Unknown suffixes ("router.internal") are fine.
  static func isUnderARegistry(_ domain: String) -> Bool {
    let labels = domain.split(separator: ".")
    guard labels.count >= 2 else { return false }
    if labels.count == 2, labels[1].count == 2, secondLevelRegistries.contains(String(labels[0])) { return false }
    return true
  }

  static let secondLevelRegistries: Set<String> = [
    "ac", "co", "com", "edu", "gob", "gov", "go", "govt", "ltd", "me", "mil", "ne", "net", "nhs", "nic", "nom",
    "or", "org", "plc", "sch", "web",
  ]

  private static func isHostCharacter(_ c: Unicode.Scalar) -> Bool {
    switch c {
    case "a"..."z", "0"..."9", "-", "_": true
    default: false
    }
  }
}

/// RFC 3492 Punycode, for IDN labels (without the "xn--" prefix).
enum Punycode {
  private static let base = 36, tMin = 1, tMax = 26, skew = 38, damp = 700
  private static let initialBias = 72, initialN = 128

  static func encode(_ input: String) -> String? {
    let scalars = input.unicodeScalars.map { Int($0.value) }
    var output = String(input.unicodeScalars.filter(\.isASCII).map(Character.init))
    let basic = output.count
    var handled = basic
    if basic > 0 { output.append("-") }
    var n = initialN, delta = 0, bias = initialBias
    while handled < scalars.count {
      guard let m = scalars.filter({ $0 >= n }).min() else { return nil }
      guard (m - n) <= (Int(Int32.max) - delta) / (handled + 1) else { return nil }
      delta += (m - n) * (handled + 1)
      n = m
      for c in scalars {
        if c < n { delta += 1 }
        if c == n {
          var q = delta
          var k = base
          while true {
            let t = k <= bias ? tMin : (k >= bias + tMax ? tMax : k - bias)
            if q < t { break }
            output.append(digit(t + (q - t) % (base - t)))
            q = (q - t) / (base - t)
            k += base
          }
          output.append(digit(q))
          bias = adapt(delta, handled + 1, first: handled == basic)
          delta = 0
          handled += 1
        }
      }
      delta += 1
      n += 1
    }
    return output
  }

  static func decode(_ input: String) -> String? {
    let chars = Array(input.unicodeScalars)
    guard chars.allSatisfy(\.isASCII) else { return nil }
    var output: [Int] = []
    var start = 0
    if let dash = chars.lastIndex(of: "-") {
      output = chars[..<dash].map { Int($0.value) }
      start = dash + 1
    }
    var n = initialN, i = 0, bias = initialBias
    var index = start
    while index < chars.count {
      let oldI = i
      var w = 1
      var k = base
      while true {
        guard index < chars.count, let d = value(chars[index]) else { return nil }
        index += 1
        guard d <= (Int(Int32.max) - i) / w else { return nil }
        i += d * w
        let t = k <= bias ? tMin : (k >= bias + tMax ? tMax : k - bias)
        if d < t { break }
        w *= base - t
        k += base
      }
      bias = adapt(i - oldI, output.count + 1, first: oldI == 0)
      n += i / (output.count + 1)
      i %= output.count + 1
      output.insert(n, at: i)
      i += 1
    }
    var result = String.UnicodeScalarView()
    for v in output {
      guard let scalar = Unicode.Scalar(UInt32(v)) else { return nil }
      result.append(scalar)
    }
    return String(result)
  }

  private static func adapt(_ delta: Int, _ points: Int, first: Bool) -> Int {
    var delta = first ? delta / damp : delta / 2
    delta += delta / points
    var k = 0
    while delta > ((base - tMin) * tMax) / 2 {
      delta /= base - tMin
      k += base
    }
    return k + (base - tMin + 1) * delta / (delta + skew)
  }

  private static func digit(_ d: Int) -> Character {
    Character(Unicode.Scalar(UInt8(d < 26 ? 97 + d : 22 + d)))
  }

  private static func value(_ c: Unicode.Scalar) -> Int? {
    switch c {
    case "a"..."z": Int(c.value) - 97
    case "A"..."Z": Int(c.value) - 65
    case "0"..."9": Int(c.value) - 22
    default: nil
    }
  }
}
