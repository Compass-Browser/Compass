// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// What Claude's Boost tools (list_boosts, save_boost, delete_boost) answer
/// and show, apart from the app: the desktop's shapes.
public enum BoostTools {
  /// The tools' switch, as the desktop's pref ("Let Claude Make Boosts").
  public static let settingKey = "compass.claude.boosts"

  public static let notPermitted =
    "Not permitted: the user hasn't turned on \"Let AI Make Boosts\" (Compass settings, AI). Tell the user if the task needs it."
  public static let denied = "The user didn't allow this Boost's JavaScript. Nothing was saved."
  public static let deleteDenied = "The user didn't allow deleting this Boost. It's still there."

  /// list_boosts: every Boost, or those on `host`, as JSON. With a host,
  /// their CSS and JavaScript too (to edit them); without, only sizes.
  public static func list(_ list: BoostList, host: String?) -> String {
    let host = host?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() ?? ""
    let boosts = host.isEmpty ? list.boosts : list.forHost(host)
    let rows: [[String: Any]] = boosts.map { boost in
      var row: [String: Any] = [
        "id": boost.id, "name": boost.name, "match": boost.match, "enabled": boost.enabled,
        "author": boost.author.rawValue, "js_world": boost.jsWorld.rawValue,
      ]
      if host.isEmpty {
        row["css_bytes"] = boost.css.utf8.count
        row["js_bytes"] = boost.js.utf8.count
      } else {
        row["css"] = boost.css
        row["js"] = boost.js
      }
      return row
    }
    var result: [String: Any] = ["boosts": rows]
    if host.isEmpty && !rows.isEmpty { result["note"] = "Pass host to see a site's CSS and JavaScript." }
    return json(result)
  }

  /// save_boost's answer.
  public static func saved(_ boost: Boost) -> String {
    json(["id": boost.id, "needs_reload": true])
  }

  /// delete_boost's answer.
  public static func deleted(_ boost: Boost) -> String {
    json(["id": boost.id, "deleted": true, "needs_reload": true])
  }

  /// The approval card's text for Claude's save: the JavaScript, as a
  /// line diff against the Boost's before (when it had some), and where
  /// it runs.
  public static func approvalDetail(_ boost: Boost, before: Boost?) -> String {
    var head = "Runs on \(boost.match)"
    head += boost.jsWorld == .main ? ", in the page's own world" : ", apart from the page's scripts"
    if let before, before.match != boost.match { head += " (was \(before.match))" }
    if let before, before.jsWorld != boost.jsWorld { head += " (world was \(before.jsWorld.rawValue))" }
    guard let before, !before.js.isEmpty, before.js != boost.js else { return head + "\n\n" + boost.js }
    return head + "\n\n" + lineDiff(from: before.js, to: boost.js)
  }

  /// Every line of `new`, with "+ " before added ones, and the removed
  /// lines of `old` with "- " where they were.
  public static func lineDiff(from old: String, to new: String) -> String {
    let a = old.components(separatedBy: "\n")
    let b = new.components(separatedBy: "\n")
    let diff = b.difference(from: a)
    var removed = Set<Int>()
    var inserted = Set<Int>()
    for change in diff {
      switch change {
      case .remove(let offset, _, _): removed.insert(offset)
      case .insert(let offset, _, _): inserted.insert(offset)
      }
    }
    var lines: [String] = []
    var i = 0
    var j = 0
    while i < a.count || j < b.count {
      if i < a.count, removed.contains(i) {
        lines.append("- " + a[i])
        i += 1
      } else if j < b.count, inserted.contains(j) {
        lines.append("+ " + b[j])
        j += 1
      } else {
        if j < b.count { lines.append("  " + b[j]) }
        i += 1
        j += 1
      }
    }
    return lines.joined(separator: "\n")
  }

  static func json(_ object: [String: Any]) -> String {
    guard
      let data = try? JSONSerialization.data(
        withJSONObject: object, options: [.sortedKeys, .withoutEscapingSlashes])
    else { return "{}" }
    return String(decoding: data, as: UTF8.self)
  }
}
