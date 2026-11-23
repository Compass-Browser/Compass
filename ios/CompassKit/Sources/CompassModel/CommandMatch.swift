// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// How the command bar matches its commands to what's typed, as the
/// desktop's (command_bar_page_handler.cc): a command shows when its name
/// contains every word, and leads the list when the words are its name's
/// first words, whole ("settings", "new incognito"; not "sett").
public enum CommandMatch {
  /// Whether `name` contains every word of `text`.
  public static func matches(_ name: String, _ text: String) -> Bool {
    let name = name.lowercased()
    return words(text).allSatisfy { name.contains($0) }
  }

  /// Whether `text` names the command `name`: its words are the name's
  /// first words, whole. A trailing ellipsis doesn't count.
  public static func names(_ name: String, _ text: String) -> Bool {
    let typed = words(text)
    var trimmed = name
    if trimmed.hasSuffix("…") { trimmed.removeLast() }
    let named = words(trimmed)
    return !typed.isEmpty && typed.count <= named.count && Array(named.prefix(typed.count)) == typed
  }

  /// `text`'s words, lowercased.
  static func words(_ text: String) -> [String] {
    text.lowercased().split(whereSeparator: \.isWhitespace).map(String.init)
  }
}
