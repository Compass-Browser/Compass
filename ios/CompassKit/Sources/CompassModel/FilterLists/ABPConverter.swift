// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Converts Adblock Plus filter lists (EasyList and friends) to WebKit
/// content-blocker rules (WKContentRuleList JSON).
///
/// WebKit can express a subset, and that's what this covers:
/// - network rules: `||domain^`, `|` anchors, `*` and `^` in patterns, with
///   `$third-party`, `$domain=`, resource types and `$match-case`;
/// - `@@` exceptions (as `ignore-previous-rules`, so they come last),
///   `@@…$document` as a whole-site exception;
/// - element hiding: generic `##selector` (batched) and per-domain
///   `a.com,~b.com##selector`.
/// Everything else (extended CSS, scriptlets, `$redirect`, `#@#`, …) is
/// skipped and counted.
public enum ABPConverter {
  public struct Result: Sendable {
    /// The rules, in order: blocking, hiding, then exceptions.
    public var rules: [[String: any Sendable]]
    public var converted: Int
    public var skipped: Int

    /// As WKContentRuleListStore wants it.
    public func json() throws -> String {
      String(decoding: try JSONSerialization.data(withJSONObject: rules), as: UTF8.self)
    }
  }

  /// Generic hiding selectors per rule: WebKit is much faster with few
  /// rules carrying many selectors than the other way round.
  static let selectorsPerRule = 250

  public static func convert(_ text: String) -> Result {
    var blocking: [[String: any Sendable]] = []
    var exceptions: [[String: any Sendable]] = []
    var hiding: [[String: any Sendable]] = []
    var generic: [String] = []
    var converted = 0
    var skipped = 0

    for raw in text.split(whereSeparator: \.isNewline) {
      let line = raw.trimmingCharacters(in: .whitespaces)
      if line.isEmpty || line.hasPrefix("!") || line.hasPrefix("[") { continue }
      guard line.allSatisfy(\.isASCII) else {
        skipped += 1
        continue
      }
      if let rule = elementHiding(line) {
        switch rule {
        case .generic(let selector):
          generic.append(selector)
        case .domains(let rule):
          hiding.append(rule)
        case .unsupported:
          skipped += 1
          continue
        }
        converted += 1
        continue
      }
      guard let (rule, isException) = network(line) else {
        skipped += 1
        continue
      }
      if isException { exceptions.append(rule) } else { blocking.append(rule) }
      converted += 1
    }

    for start in stride(from: 0, to: generic.count, by: selectorsPerRule) {
      let batch = generic[start..<min(start + selectorsPerRule, generic.count)]
      hiding.append([
        "trigger": ["url-filter": ".*"] as [String: any Sendable],
        "action": ["type": "css-display-none", "selector": batch.joined(separator: ", ")] as [String: any Sendable],
      ])
    }
    return Result(rules: blocking + hiding + exceptions, converted: converted, skipped: skipped)
  }

  // MARK: Element hiding

  enum Hiding {
    case generic(String)
    case domains([String: any Sendable])
    case unsupported
  }

  /// nil: not an element-hiding line at all.
  static func elementHiding(_ line: String) -> Hiding? {
    // Exceptions (#@#), extended CSS (#?#), snippets (#$#), scriptlets
    // (##+js) and HTML filters (##^) aren't for WebKit.
    for marker in ["#@#", "#?#", "#$#", "#@$#", "#@?#", "#%#", "$$"] where line.contains(marker) {
      return .unsupported
    }
    guard let range = line.range(of: "##") else { return nil }
    let selector = String(line[range.upperBound...])
    let domains = String(line[..<range.lowerBound])
    if selector.isEmpty || selector.hasPrefix("+js") || selector.hasPrefix("^") { return .unsupported }
    // Procedural pseudo-classes WebKit doesn't know.
    for pseudo in [":-abp-", ":has-text(", ":xpath(", ":contains(", ":matches-css", ":upward(", ":remove(", ":style("]
    where selector.contains(pseudo) {
      return .unsupported
    }
    if domains.isEmpty { return .generic(selector) }
    let (included, excluded) = splitDomains(domains.split(separator: ",").map(String.init))
    guard let trigger = domainTrigger(included: included, excluded: excluded) else { return .unsupported }
    var urlTrigger = trigger
    urlTrigger["url-filter"] = ".*"
    return .domains([
      "trigger": urlTrigger,
      "action": ["type": "css-display-none", "selector": selector] as [String: any Sendable],
    ])
  }

  // MARK: Network

  static let resourceTypes: [String: [String]] = [
    "script": ["script"], "image": ["image"], "stylesheet": ["style-sheet"], "font": ["font"],
    "media": ["media"], "xmlhttprequest": ["fetch"], "subdocument": ["document"],
    "popup": ["popup"], "ping": ["ping"], "websocket": ["websocket"], "object": ["other"],
    "other": ["other"],
  ]
  static let allTypes = ["script", "image", "style-sheet", "font", "media", "fetch", "document", "popup", "ping", "websocket", "other"]
  /// Options that change nothing WebKit can do differently.
  static let ignoredOptions: Set<String> = ["important", "~important", "third-party=false"]

  /// The rule, and whether it's an exception. nil: unsupported.
  static func network(_ line: String) -> ([String: any Sendable], Bool)? {
    var pattern = Substring(line)
    let isException = pattern.hasPrefix("@@")
    if isException { pattern = pattern.dropFirst(2) }
    var options: [String] = []
    if let dollar = pattern.lastIndex(of: "$") {
      options = pattern[pattern.index(after: dollar)...].split(separator: ",").map { $0.lowercased() }
      pattern = pattern[..<dollar]
    }
    // A regex rule (/…/): WebKit's regex subset can't take most of them.
    if pattern.count > 1, pattern.hasPrefix("/"), pattern.hasSuffix("/") { return nil }
    var trigger: [String: any Sendable] = [:]
    var types: Set<String>?
    var negatedTypes: Set<String> = []
    var included: [String] = []
    var excluded: [String] = []
    var wholeDocument = false
    for option in options {
      if ignoredOptions.contains(option) { continue }
      switch option {
      case "third-party", "3p":
        trigger["load-type"] = ["third-party"]
      case "~third-party", "1p", "first-party":
        trigger["load-type"] = ["first-party"]
      case "match-case":
        trigger["url-filter-is-case-sensitive"] = true
      case "document", "doc":
        guard isException else { return nil }
        wholeDocument = true
      default:
        if option.hasPrefix("domain=") {
          let list = option.dropFirst(7).split(separator: "|").map(String.init)
          (included, excluded) = splitDomains(list)
        } else if option.hasPrefix("~"), let mapped = resourceTypes[String(option.dropFirst())] {
          negatedTypes.formUnion(mapped)
        } else if let mapped = resourceTypes[option] {
          types = (types ?? []).union(mapped)
        } else {
          // $redirect, $csp, $removeparam, $elemhide, $generichide, …
          return nil
        }
      }
    }
    if let types {
      trigger["resource-type"] = Array(types).sorted()
    } else if !negatedTypes.isEmpty {
      trigger["resource-type"] = allTypes.filter { !negatedTypes.contains($0) }
    }
    if !included.isEmpty || !excluded.isEmpty {
      guard let domains = domainTrigger(included: included, excluded: excluded) else { return nil }
      trigger.merge(domains) { a, _ in a }
    }

    let action: [String: any Sendable] = ["type": isException ? "ignore-previous-rules" : "block"]
    if wholeDocument {
      // @@||example.com^$document: nothing blocked on that site.
      guard let host = plainHost(pattern) else { return nil }
      trigger["url-filter"] = ".*"
      trigger["if-domain"] = ["*" + host]
      trigger["load-type"] = nil
      return (["trigger": trigger, "action": action], true)
    }
    guard let regex = regex(pattern) else { return nil }
    trigger["url-filter"] = regex
    return (["trigger": trigger, "action": action], isException)
  }

  /// "||example.com^" → "example.com", for whole-site exceptions.
  static func plainHost(_ pattern: Substring) -> String? {
    var p = pattern
    guard p.hasPrefix("||") else { return nil }
    p = p.dropFirst(2)
    if p.hasSuffix("^") { p = p.dropLast() }
    let host = p.lowercased()
    return !host.isEmpty && host.allSatisfy({ $0.isLetter || $0.isNumber || $0 == "." || $0 == "-" }) ? host : nil
  }

  /// An ABP pattern as a WebKit url-filter regex (its subset: no "|", no
  /// "{}", no backslash classes).
  static func regex(_ pattern: Substring) -> String? {
    var p = pattern
    var out = ""
    if p.hasPrefix("||") {
      out = "^[a-z][a-z0-9.+-]*://([^/?#]*\\.)?"
      p = p.dropFirst(2)
    } else if p.hasPrefix("|") {
      out = "^"
      p = p.dropFirst()
    }
    var anchoredEnd = false
    if p.hasSuffix("|") {
      anchoredEnd = true
      p = p.dropLast()
    }
    while p.hasPrefix("*") { p = p.dropFirst() }
    while p.hasSuffix("*") { p = p.dropLast() }
    // An empty or tiny pattern would match everything.
    if p.count < 3 { return nil }
    let separator = "[/:?=&#]"
    let chars = Array(p)
    for (i, c) in chars.enumerated() {
      switch c {
      case "*":
        out += ".*"
      case "^":
        // At the end, a separator or the end of the address.
        out += i == chars.count - 1 && !anchoredEnd ? "(\(separator).*)?$" : separator
      case ".", "+", "?", "$", "(", ")", "[", "]", "/", "\\":
        out += "\\" + String(c)
      case "{", "}", "|":
        return nil
      default:
        out += String(c)
      }
    }
    if anchoredEnd { out += "$" }
    return out
  }

  // MARK: Domains

  static func splitDomains(_ list: [String]) -> ([String], [String]) {
    var included: [String] = []
    var excluded: [String] = []
    for d in list {
      let domain = d.trimmingCharacters(in: .whitespaces).lowercased()
      if domain.hasPrefix("~") { excluded.append(String(domain.dropFirst())) } else if !domain.isEmpty { included.append(domain) }
    }
    return (included, excluded)
  }

  /// WebKit takes if-domain or unless-domain, not both; "*" covers
  /// subdomains. Wildcard TLDs (example.*) can't be said.
  static func domainTrigger(included: [String], excluded: [String]) -> [String: any Sendable]? {
    let valid = { (d: String) in !d.contains("*") && !d.contains("/") && d.contains(".") }
    if !included.isEmpty {
      // Exclusions inside inclusions (a.com,~b.a.com) can't be said; the
      // rule then applies to the included domains as a whole.
      let domains = included.filter(valid)
      return domains.isEmpty ? nil : ["if-domain": domains.map { "*" + $0 }]
    }
    if !excluded.isEmpty {
      let domains = excluded.filter(valid)
      return domains.isEmpty ? nil : ["unless-domain": domains.map { "*" + $0 }]
    }
    return [:]
  }
}
