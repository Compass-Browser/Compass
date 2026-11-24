// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Observation

/// Shorter tab titles (docs/design/ai-sidebar.md, "Shorter tab titles"; the
/// desktop's short titles): a tab whose title is long, or ends in a site's
/// name (" · GitHub", " - YouTube"), reads as a short one in the sidebar's
/// rows, "Pull request #123 · org/repo · GitHub" → "PR #123: fix login".
///
/// Display only: never saved or synced, and nothing else (the page menu,
/// the command bar, the chat) shows it. A user's own name for a tab
/// (`customTitle`) always wins. Titles shown in the last `batchWindow` go
/// in one background request (`ClaudeQuick`: the hourly limit and the
/// back-off apply), up to `maxBatch` a request; answers are kept per
/// (host, title) while the app runs, so a page seen again costs nothing.
/// Only with "Shorter Tab Titles" on (and Claude set up); never in
/// incognito.
@MainActor
@Observable
public final class TabTitleShortener {
  /// How long titles gather before they're sent.
  public static let batchWindow: Duration = .seconds(10)
  /// The most titles a request lists.
  public static let maxBatch = 20
  /// A title longer than this (in characters) is shortened.
  public static let longTitle = 40
  /// The longest site's name `isLong()` takes for one (" · GitHub").
  public static let maxSiteSuffix = 30
  /// The prompts file's section for these requests.
  public static let promptID = "short_titles"
  /// The answers kept; past that the cache starts over.
  public static let maxCache = 2000
  /// A title this long (in UTF-8 bytes) says enough to Claude.
  static let maxTitleLength = 200

  /// A page as the cache knows it: its host and its title.
  public struct Key: Hashable, Sendable {
    public var host: String
    public var title: String

    public init(host: String, title: String) {
      self.host = host
      self.title = title
    }
  }

  /// Claude's short titles; "" for a title Claude had nothing shorter for
  /// (it's not asked again).
  public private(set) var cache: [Key: String] = [:]

  /// How long titles gather before they're sent (`batchWindow`; tests
  /// shorten it).
  @ObservationIgnored public var window: Duration = TabTitleShortener.batchWindow
  /// Titles waiting for the next request, in the order they came.
  @ObservationIgnored private(set) var pending: [Key] = []
  /// Titles in a request on its way.
  @ObservationIgnored private(set) var asking: Set<Key> = []
  /// The window's wait; nil once it's over.
  @ObservationIgnored private var timer: Task<Void, Never>?
  /// Requests are going out (`flush()`).
  @ObservationIgnored private var flushing = false

  let store: SpaceStore
  let quick: ClaudeQuick
  /// common/claude/sidebar_prompts.md's text.
  let prompts: String
  /// The model "Shorter Tab Titles" asks.
  let model: @MainActor () -> String
  /// The browser's language, a BCP 47 tag ("it-IT").
  let locale: @MainActor () -> String

  public init(
    store: SpaceStore, quick: ClaudeQuick, prompts: String, model: @escaping @MainActor () -> String,
    locale: @escaping @MainActor () -> String = { Locale.preferredLanguages.first ?? "en-US" }
  ) {
    self.store = store
    self.quick = quick
    self.prompts = prompts
    self.model = model
    self.locale = locale
  }

  /// Whether "Shorter Tab Titles" is on and Claude set up.
  public var isEnabled: Bool { quick.isEnabled(ClaudeQuick.shortTabTitlesKey) }

  /// `tab`'s key: its host and its page's title.
  public static func key(_ tab: SidebarNode) -> Key {
    Key(host: URL(string: tab.url)?.host() ?? "", title: tab.title)
  }

  /// Whether `tab` may show a short title: a tab in Today or Pinned (not
  /// Favorites, whose tiles show no title) with the user's name for it
  /// unset, outside incognito, with a title worth shortening. The
  /// desktop's ShortTitler::IsEligible().
  func isEligible(_ tab: SidebarNode) -> Bool {
    tab.isTab && (tab.section == .today || tab.section == .pinned) && tab.customTitle.isEmpty
      && !store.isIncognito(tab) && Self.isLong(tab.title)
  }

  /// The title the sidebar's row shows for `tab` instead of its page's
  /// title; nil to show that one.
  public func shortTitle(for tab: SidebarNode) -> String? {
    guard isEnabled, isEligible(tab), let short = cache[Self.key(tab)], !short.isEmpty else { return nil }
    return short
  }

  /// `tab`'s row is shown, or its title changed: its title goes in the next
  /// request unless it's known already.
  public func want(_ tab: SidebarNode) {
    guard isEnabled, isEligible(tab) else { return }
    let key = Self.key(tab)
    guard cache[key] == nil, !asking.contains(key), !pending.contains(key) else { return }
    pending.append(key)
    // 20 waiting go at once (the desktop's kMaxBatch).
    if pending.count >= Self.maxBatch {
      // Only the window's wait is cancelled, never a request: a cancelled
      // task's request would give up at once.
      timer?.cancel()
      timer = nil
      if !flushing { Task { [weak self] in await self?.flush() } }
      return
    }
    guard timer == nil else { return }
    let window = window
    timer = Task { [weak self] in
      try? await Task.sleep(for: window)
      guard !Task.isCancelled else { return }
      await self?.flush()
    }
  }

  /// Sends what waits, `maxBatch` titles a request.
  func flush() async {
    timer = nil
    guard !flushing else { return }  // the loop under way takes them
    guard isEnabled else {
      pending = []
      return
    }
    flushing = true
    defer { flushing = false }
    while !pending.isEmpty {
      let batch = Array(pending.prefix(Self.maxBatch))
      pending.removeFirst(batch.count)
      guard await send(batch) else {
        // Off, not set up, over the hour's limit or waiting after an
        // error: the rest waits until its row shows again.
        pending = []
        return
      }
    }
  }

  /// One request for `batch`: false if it couldn't go (or failed), so the
  /// rest isn't sent now. As the desktop's ShortTitler::OnReply().
  private func send(_ batch: [Key]) async -> Bool {
    asking.formUnion(batch)
    let request = ClaudeQuick.Request(
      feature: ClaudeQuick.shortTabTitlesKey, model: model(),
      system: SidebarPrompts.section(Self.promptID, in: prompts, locale: locale()), user: Self.describe(batch),
      maxTokens: ClaudeQuick.maxTokens, background: true
    ).withOnDevicePrompt(Self.promptID, prompts: prompts, locale: locale())
    let result = await quick.ask(request)
    asking.subtract(batch)
    switch result {
    case .success(let reply):
      let titles = parseShortTitlesReply(reply, ids: batch.indices.map { String($0 + 1) })
      if cache.count + batch.count > Self.maxCache { cache = [:] }
      for (i, key) in batch.enumerated() {
        // A title without a shorter one isn't asked again this run.
        let short = titles[String(i + 1)].flatMap { Self.accept($0, for: key.title) }
        cache[key] = short ?? ""
      }
      return true
    case .failure:
      // Off, the hourly limit, the back-off, an error or a reply that
      // isn't titles: nothing is kept.
      return false
    }
  }

  /// `short` if it's shorter than `title`, else nil.
  static func accept(_ short: String, for title: String) -> String? {
    short.count < title.count && short != title ? short : nil
  }

  /// The user message: the titles, a line each, "<n>. <title> (<host>)"
  /// (the desktop's DescribeTitles()). Never paths, queries or what pages
  /// hold.
  public static func describe(_ batch: [Key]) -> String {
    var text = "The tabs:\n"
    for (i, key) in batch.enumerated() {
      var title = key.title.replacingOccurrences(of: "\r", with: " ").replacingOccurrences(of: "\n", with: " ")
        .replacingOccurrences(of: "\t", with: " ").trimmingCharacters(in: .whitespaces)
      if title.utf8.count > maxTitleLength {
        var cut = ""
        for character in title {
          if cut.utf8.count + String(character).utf8.count > maxTitleLength { break }
          cut.append(character)
        }
        title = cut + "\u{2026}"
      }
      text += "\(i + 1). \(title)"
      if !key.host.isEmpty { text += " (\(key.host))" }
      text += "\n"
    }
    return text
  }

  /// Whether `title` is worth shortening: longer than `longTitle`, or
  /// ending in a site's name (at most `maxSiteSuffix` characters) after
  /// " · ", " - ", " | ", " – ", " — " or " :: ". The desktop's
  /// NeedsShortTitle().
  public static func isLong(_ title: String) -> Bool {
    let title = title.trimmingCharacters(in: .whitespacesAndNewlines)
    if title.count > longTitle { return true }
    for separator in [" · ", " - ", " | ", " – ", " — ", " :: "] {
      if let range = title.range(of: separator, options: .backwards) {
        let site = title[range.upperBound...]
        let page = title[..<range.lowerBound]
        if !site.isEmpty, site.count <= maxSiteSuffix, !page.trimmingCharacters(in: .whitespaces).isEmpty { return true }
      }
    }
    return false
  }
}

// MARK: The reply

/// A short title Claude gives: at most this many characters.
public let maxShortTitleLength = 40

/// The short titles of a reply `{"titles": {"<id>": "<title>", …}}`, by id:
/// ids not in `ids` are dropped, and titles `sanitizeShortTitle()` drops.
/// Empty for a reply of another shape.
public func parseShortTitlesReply(_ reply: [String: JSONValue], ids: [String]) -> [String: String] {
  guard let titles = reply["titles"]?.object else { return [:] }
  let known = Set(ids)
  var result: [String: String] = [:]
  for (id, value) in titles where known.contains(id) {
    if let text = value.string, let title = sanitizeShortTitle(text) { result[id] = title }
  }
  return result
}

/// `title` as the sidebar may show it in place of a tab's title: trimmed
/// (also of quotes around it and a full stop at the end), its spaces
/// collapsed, 1 to `maxShortTitleLength` characters with a letter or digit,
/// and none of what `sanitizeAiName()` refuses (emoji, control characters,
/// line breaks, markup). Any number of words: a title may read as a phrase
/// ("PR #123: fix login"). Nil if it can't be one. The desktop's
/// SanitizeShortTitle().
public func sanitizeShortTitle(_ title: String) -> String? {
  if title.unicodeScalars.contains(where: { $0 == "\r" || $0 == "\n" }) { return nil }
  var text = title.trimmingCharacters(in: .whitespaces)
  let quotes: Set<Unicode.Scalar> = ["\"", "'", "`", "\u{201C}", "\u{201D}", "\u{2018}", "\u{2019}", "\u{00AB}", "\u{00BB}"]
  while text.unicodeScalars.count >= 2, let first = text.unicodeScalars.first, let last = text.unicodeScalars.last,
    quotes.contains(first), quotes.contains(last)
  {
    text = String(Substring(text.unicodeScalars.dropFirst().dropLast())).trimmingCharacters(in: .whitespaces)
  }
  while let last = text.unicodeScalars.last, last == "." || last == "\u{3002}" {
    text = String(Substring(text.unicodeScalars.dropLast()))
  }
  text = text.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
  guard !text.isEmpty else { return nil }
  var hasAlnum = false
  for scalar in text.unicodeScalars {
    if isRefusedInName(scalar) { return nil }
    switch scalar.properties.generalCategory {
    case .uppercaseLetter, .lowercaseLetter, .titlecaseLetter, .modifierLetter, .otherLetter, .decimalNumber:
      hasAlnum = true
    default:
      break
    }
  }
  return hasAlnum && text.count <= maxShortTitleLength ? text : nil
}
