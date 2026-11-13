// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// iOS's inspect_element, page_storage and performance answers, worded as
/// the desktop's (browser/claude/claude_devtools_tools.cc), from what
/// `DevToolsScripts` read. What WebKit can't give says "unavailable".
public enum DevToolsReports {
  static let maxRules = 20
  static let maxInspectChars = 15000
  static let maxStorageItems = 200
  static let maxStorageValue = 300
  static let maxShownStorageValue = 2000

  // MARK: inspect_element

  /// `inspected` from `DevToolsScripts.inspect`; `listeners` from the
  /// capture script (nil when it isn't in the page).
  public static func inspect(_ inspected: JSONValue, selector: String, listeners: JSONValue?, userAgent: Bool) -> String {
    var text = "\(inspected["node"]?.string ?? "The element")  (\(selector))\n\(inspected["box"]?.string ?? "")\n\nComputed styles:\n"
    for pair in inspected["computed"]?.array ?? [] {
      guard let list = pair.array, list.count == 2, let name = list[0].string else { continue }
      text += "  \(name): \(DevToolsFormat.cut(list[1].string ?? "", 200))\n"
    }
    text += "\nMatching rules, winning first (by specificity and order; lines unavailable on iOS):\n"
    var rules = 0
    let inline = declarations(inspected["inline"])
    if !inline.isEmpty {
      text += "  element.style (its style attribute)\n" + inline
      rules += 1
    }
    for rule in inspected["rules"]?.array ?? [] {
      if rules == maxRules {
        text += "  … more rules not shown.\n"
        break
      }
      let media = rule["media"]?.string.flatMap { $0.isEmpty ? nil : " @media \($0)" } ?? ""
      text += "  \(rule["selector"]?.string ?? "?")\(media)  (\(rule["where"]?.string ?? "a style sheet"))\n"
      text += declarations(rule["declarations"])
      rules += 1
    }
    if rules == 0 { text += "  none of the page's own.\n" }
    if userAgent { text += "  (The browser's default rules: unavailable on iOS.)\n" }
    let blocked = (inspected["blocked"]?.array ?? []).compactMap(\.string)
    if !blocked.isEmpty {
      text += "  Rules of other sites' style sheets can't be read on iOS: \(blocked.prefix(5).joined(separator: ", "))\n"
    }
    text += "\nEvent listeners on the element:\n"
    if let list = listeners?.array {
      if list.isEmpty {
        text += "  none (listeners on ancestors, like the document, handle its events too: inspect those).\n"
      }
      for listener in list {
        var flags: [String] = []
        if listener["capture"] == .bool(true) { flags.append("capture") }
        if listener["passive"] == .bool(true) { flags.append("passive") }
        if listener["once"] == .bool(true) { flags.append("once") }
        if listener["inline"] == .bool(true) { flags.append("inline attribute") }
        let handler = listener["handler"]?.string ?? ""
        text += "  \(listener["type"]?.string ?? "?")\(flags.isEmpty ? "" : " (\(flags.joined(separator: ", ")))")"
        text += handler.isEmpty ? "\n" : " — \(DevToolsFormat.cut(handler, 150))\n"
      }
      text += "  (On iOS, only listeners added since the page loaded with the developer tools on.)\n"
    } else {
      text += "  unavailable: the page loaded before the developer tools were on; reload it to see them.\n"
    }
    return DevToolsFormat.cut(text, maxInspectChars)
  }

  private static func declarations(_ value: JSONValue?) -> String {
    var text = ""
    for item in value?.array ?? [] {
      guard let list = item.array, list.count == 3, let name = list[0].string else { continue }
      let important = list[2].string == "important" ? " !important" : ""
      text += "    \(name): \(DevToolsFormat.cut(list[1].string ?? "", 200))\(important);\n"
    }
    return text
  }

  // MARK: page_storage

  public struct StorageRequest: Sendable {
    public var cookies = true
    public var local = true
    public var session = true
    public var indexedDB = true
    /// Lowercased; empty for all.
    public var filter = ""
    public var showValues = false

    public init(store: String?, filter: String?, showValues: Bool) {
      if let store {
        cookies = store == "cookies"
        local = store == "localStorage"
        session = store == "sessionStorage"
        indexedDB = store == "indexedDB"
      }
      self.filter = (filter ?? "").lowercased()
      self.showValues = showValues
    }

    func matches(_ name: String) -> Bool { filter.isEmpty || name.lowercased().contains(filter) }
  }

  /// `storage` from the page agent's storage(); `indexedDB` from
  /// `DevToolsScripts.indexedDBNames`.
  public static func storage(
    origin: String, request: StorageRequest, cookies: [DevToolsFormat.Cookie]?, storage: JSONValue?,
    indexedDB: JSONValue?
  ) -> String {
    var text = "Storage of \(origin)\n"
    if request.cookies {
      if let cookies {
        let matching = cookies.filter { request.matches($0.name) }
        text += "\nCookies (\(matching.count)):\n"
        text += matching.isEmpty ? "  none\n" : matching.map { $0.line(showValues: request.showValues) }.joined()
      } else {
        text += "\nCookies: couldn't read them.\n"
      }
    }
    for (name, wanted) in [("localStorage", request.local), ("sessionStorage", request.session)] where wanted {
      guard let store = storage?[name] else {
        text += "\n\(name): couldn't read it.\n"
        continue
      }
      if let error = store["error"]?.string {
        text += "\n\(name): the page can't use it (\(error)).\n"
        continue
      }
      var lines = ""
      var matching = 0
      var shown = 0
      for pair in store["items"]?.array ?? [] {
        guard let list = pair.array, list.count == 2, let key = list[0].string else { continue }
        let value = list[1].string ?? ""
        guard request.matches(key) else { continue }
        matching += 1
        guard shown < maxStorageItems else { continue }
        shown += 1
        let shownValue = request.showValues
          ? DevToolsFormat.cut(value, maxShownStorageValue)
          : DevToolsFormat.isSecretName(key) || DevToolsFormat.looksSecret(value)
            ? DevToolsFormat.hidden(value) : DevToolsFormat.cut(DevToolsFormat.maskSecretsInText(value), maxStorageValue)
        lines += "  \(key) = \(shownValue)\n"
      }
      text += "\n\(name) (\(matching) \(matching == 1 ? "key" : "keys")):\n" + (matching > 0 ? lines : "  none\n")
      if matching > shown { text += "  … \(matching - shown) more: use filter.\n" }
    }
    if request.indexedDB {
      if let names = indexedDB?["names"]?.array {
        let list = names.compactMap(\.string)
        text += "\nIndexedDB databases: \(list.isEmpty ? "none" : list.joined(separator: ", "))\n"
      } else {
        text += "\nIndexedDB: couldn't list them (\(indexedDB?["error"]?.string ?? "unavailable")).\n"
      }
    }
    return text
  }

  // MARK: performance

  /// `vitals` from `DevToolsScripts.performance`.
  public static func performance(_ vitals: JSONValue?) -> String {
    guard let v = vitals, v.object != nil else { return "The page's timings couldn't be read.\n" }
    func num(_ key: String) -> Double { v[key]?.number ?? -1 }
    func supported(_ key: String) -> Bool { v[key] != .bool(false) }
    let unavailable = "unavailable (WebKit doesn't report it)"
    var text = "Performance of \(v["url"]?.string ?? "")\n\nLoading:\n"
    let ttfb = num("ttfb")
    text += "  Time to first byte: \(DevToolsFormat.ms(ttfb))\(ttfb >= 0 ? " (\(DevToolsFormat.rate(ttfb, good: 800, poor: 1800)))" : "")\n"
    let fcp = num("fcp")
    text += "  First contentful paint: \(DevToolsFormat.ms(fcp))\(fcp >= 0 ? " (\(DevToolsFormat.rate(fcp, good: 1800, poor: 3000)))" : "")\n"
    if supported("lcpSupported") {
      let lcp = num("lcp")
      let element = v["lcpElement"]?.string ?? ""
      text += "  Largest contentful paint: \(DevToolsFormat.ms(lcp))\(lcp >= 0 ? " (\(DevToolsFormat.rate(lcp, good: 2500, poor: 4000)))" : "")\(element.isEmpty ? "" : ", \(element)")\n"
    } else {
      text += "  Largest contentful paint: \(unavailable)\n"
    }
    text += "  DOMContentLoaded: \(DevToolsFormat.ms(num("dcl"))); load: \(DevToolsFormat.ms(num("load")))\n"
    text += "\nStability and responsiveness:\n"
    if supported("clsSupported") {
      let cls = max(0, num("cls"))
      text += String(format: "  Cumulative layout shift: %.3f", cls) + " (\(DevToolsFormat.rate(cls, good: 0.1, poor: 0.25)))\n"
    } else {
      text += "  Cumulative layout shift: \(unavailable)\n"
    }
    if supported("inpSupported") {
      let inp = num("inp")
      let count = Int(v["interactions"]?.number ?? 0)
      text += "  Interaction to next paint: "
      text += inp >= 0
        ? "\(DevToolsFormat.ms(inp)) (\(DevToolsFormat.rate(inp, good: 200, poor: 500))), the slowest of \(count) \(count == 1 ? "interaction" : "interactions")\n"
        : "no interactions yet\n"
    } else {
      text += "  Interaction to next paint: \(unavailable)\n"
    }
    text += supported("framesSupported")
      ? "  Long animation frames: \(Int(v["longFrames"]?.number ?? 0)), blocking \(DevToolsFormat.ms(max(0, num("blocking"))))\n"
      : "  Long animation frames: \(unavailable)\n"
    text += "\nResources: \(Int(v["resources"]?.number ?? 0)), \(DevToolsFormat.formatBytes(max(0, num("transfer")))) transferred"
    let slowest = v["slowest"]?.array ?? []
    if slowest.isEmpty {
      text += "\n"
    } else {
      text += "; the slowest:\n"
      for resource in slowest {
        text += "  \(DevToolsFormat.ms(resource["duration"]?.number ?? 0))  \(resource["type"]?.string ?? "")  \(NetworkListing.listedURL(resource["url"]?.string ?? ""))\n"
      }
    }
    text += "\nNow:\n  DOM nodes: \(Int(v["nodes"]?.number ?? 0))\n"
    text += "  JavaScript heap, layouts, style recalculations and script time: \(unavailable)\n"
    return text
  }
}
