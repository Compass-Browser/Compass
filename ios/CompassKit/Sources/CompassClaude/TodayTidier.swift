// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Observation

/// Tidy Today (docs/design/ai-sidebar.md, "Tidy Today"; the desktop's
/// TodayTidier): one request with a space's loose Today entries (tabs and
/// splits at Today's top, a split as one; folders and their contents left
/// alone), whose reply becomes folders named by Claude (`.claude`) at the
/// place of each group's topmost entry, in one change; Undo puts every
/// entry back and removes the folders, in one change too.
///
/// Only with "Tidy Today" on (and Claude set up: `ClaudeQuick`); never in
/// incognito. Tidy is the user's request: no hourly limit, no back-off.
@MainActor
@Observable
public final class TodayTidier {
  /// Fewer loose entries than this aren't worth tidying.
  public static let minEntries = 5
  /// The most entries a request lists, from Today's top.
  public static let maxEntries = 40
  /// The tidies Undo can still take back (not saved).
  public static let maxUndos = 10
  /// The prompts file's section for these requests.
  public static let promptID = "tidy_today"
  /// A title this long (in UTF-8 bytes) says enough.
  static let maxTitleLength = 100

  /// How a tidy ended, for the toast.
  public enum Outcome: Equatable, Sendable {
    /// `folders` folders made; `undo` takes them back (`undo(_:)`).
    case tidied(undo: Int, folders: Int)
    /// Nothing changed, and why ("Couldn't tidy Today: …").
    case failed(String)
  }

  /// Spaces (their IDs) with a tidy on its way: their header says
  /// "Tidying…".
  public private(set) var busy: Set<String> = []

  let store: SpaceStore
  let quick: ClaudeQuick
  /// common/claude/sidebar_prompts.md's text.
  let prompts: String
  /// The model "Tidy Today" asks.
  let model: @MainActor () -> String
  /// The browser's language, a BCP 47 tag ("it-IT").
  let locale: @MainActor () -> String
  /// Whether the sidebar shows an entry (the phone hides the desktop's own
  /// pages): only those are tidied.
  let isShown: @MainActor (SidebarNode) -> Bool
  @ObservationIgnored var undos: [(id: Int, tidy: SpaceStore.TodayTidy)] = []
  @ObservationIgnored var nextUndoID = 1

  public init(
    store: SpaceStore, quick: ClaudeQuick, prompts: String, model: @escaping @MainActor () -> String,
    locale: @escaping @MainActor () -> String = { Locale.preferredLanguages.first ?? "en-US" },
    isShown: @escaping @MainActor (SidebarNode) -> Bool = { _ in true }
  ) {
    self.store = store
    self.quick = quick
    self.prompts = prompts
    self.model = model
    self.locale = locale
    self.isShown = isShown
  }

  /// Whether "Tidy Today" is on and Claude set up.
  public var isEnabled: Bool { quick.isEnabled(ClaudeQuick.tidyTodayKey) }

  /// Whether Today's header offers Tidy in `space`: the switch on, Claude
  /// set up, not incognito.
  public func isAvailable(in space: Space) -> Bool { !space.isIncognito && isEnabled }

  public func isBusy(_ space: Space) -> Bool { busy.contains(space.id) }

  /// `space`'s loose Today entries, from Today's top (the end of its
  /// children: the sidebar shows Today newest first).
  public func looseEntries(of space: Space) -> [SidebarNode] {
    space.today.children.reversed().filter { ($0.isTab || $0.isSplit) && isShown($0) }
  }

  /// Whether Tidy may run in `space` now: available, not on its way, and
  /// `minEntries` loose entries.
  public func canTidy(_ space: Space) -> Bool {
    isAvailable(in: space) && !isBusy(space) && looseEntries(of: space).count >= Self.minEntries
  }

  /// The user message: the entries, a line each, "<n>. <title> (<host>)",
  /// a split's titles joined by " + " (the desktop's DescribeEntries()).
  /// Never paths, queries or what pages hold.
  public static func describe(_ entries: [SidebarNode]) -> String {
    var text = "Today's loose entries:\n"
    for (i, entry) in entries.enumerated() {
      let tabs = entry.isSplit ? entry.children : [entry]
      var titles: [String] = []
      var hosts: [String] = []
      for tab in tabs {
        let host = URL(string: tab.url)?.host() ?? ""
        var title = lineTitle(tab)
        if title.isEmpty { title = host }
        if !title.isEmpty { titles.append(title) }
        if !host.isEmpty && !hosts.contains(host) { hosts.append(host) }
      }
      text += "\(i + 1). " + (titles.isEmpty ? "(untitled)" : titles.joined(separator: " + "))
      if !hosts.isEmpty { text += " (" + hosts.joined(separator: ", ") + ")" }
      text += "\n"
    }
    return text
  }

  /// `tab`'s title as one line, trimmed and shortened.
  static func lineTitle(_ tab: SidebarNode) -> String {
    var title = (tab.customTitle.isEmpty ? tab.title : tab.customTitle)
      .replacingOccurrences(of: "\r", with: " ").replacingOccurrences(of: "\n", with: " ")
      .replacingOccurrences(of: "\t", with: " ")
      .trimmingCharacters(in: .whitespaces)
    if title.utf8.count > maxTitleLength {
      var cut = ""
      for character in title {
        if cut.utf8.count + String(character).utf8.count > maxTitleLength { break }
        cut.append(character)
      }
      title = cut + "\u{2026}"
    }
    return title
  }

  /// Tidies `space`'s Today, as the user asked: nil if it can't now (or
  /// the switch went off while Claude answered), else how it ended.
  @discardableResult
  public func tidy(_ space: Space) async -> Outcome? {
    guard canTidy(space) else { return nil }
    let entries = Array(looseEntries(of: space).prefix(Self.maxEntries))
    let ids = entries.map(\.id)
    let request = ClaudeQuick.Request(
      feature: ClaudeQuick.tidyTodayKey, model: model(),
      system: SidebarPrompts.section(Self.promptID, in: prompts, locale: locale()), user: Self.describe(entries),
      maxTokens: ClaudeQuick.maxTokens,
      // The user asked: no hourly limit, no waiting out a back-off.
      background: false
    ).withOnDevicePrompt(Self.promptID, prompts: prompts, locale: locale())
    let spaceID = space.id
    busy.insert(spaceID)
    let result = await quick.ask(request)
    busy.remove(spaceID)
    guard let space = store.space(id: spaceID), !space.isIncognito else { return nil }
    let reply: [String: JSONValue]
    switch result {
    case .success(let value): reply = value
    // Turned off meanwhile: nothing to say.
    case .failure(.off): return nil
    case .failure(.notSetUp): return .failed("Couldn't tidy Today: Claude isn't set up.")
    case .failure(.timeout): return .failed("Couldn't tidy Today: Claude took too long.")
    case .failure(.failed), .failure(.rateLimited), .failure(.backingOff):
      return .failed("Couldn't tidy Today: Claude couldn't be reached.")
    case .failure(.refused), .failure(.badReply): return .failed("Couldn't tidy Today.")
    }
    // Turned off while Claude thought: nothing changes.
    guard isEnabled else { return nil }
    return apply(reply, ids: ids, in: space)
  }

  /// Makes the folders `reply` asks for, of the entries `ids` (listed as 1
  /// to n). Entries closed or moved meanwhile are skipped.
  func apply(_ reply: [String: JSONValue], ids: [String], in space: Space) -> Outcome {
    let numbers = ids.indices.map { String($0 + 1) }
    let groups = parseTidyReply(reply, ids: numbers).map { folder in
      SpaceStore.TidyGroup(name: folder.name, entries: folder.entries.compactMap { Int($0).map { ids[$0 - 1] } })
    }
    let tidy = store.tidyToday(in: space, groups: groups)
    guard !tidy.folders.isEmpty else { return .failed("Claude found nothing to group.") }
    let id = nextUndoID
    nextUndoID += 1
    undos.append((id, tidy))
    if undos.count > Self.maxUndos { undos.removeFirst(undos.count - Self.maxUndos) }
    return .tidied(undo: id, folders: tidy.folders.count)
  }

  /// Whether `undo(id)` can still take a tidy back.
  public func canUndo(_ id: Int) -> Bool { undos.contains { $0.id == id } }

  /// Takes the tidy `id` back (the toast's Undo): every entry back in its
  /// place, the folders gone. Once.
  @discardableResult
  public func undo(_ id: Int) -> Bool {
    guard let index = undos.firstIndex(where: { $0.id == id }) else { return false }
    let tidy = undos.remove(at: index).tidy
    return store.undoTidy(tidy)
  }

  /// The toast's text for a tidy.
  public static func text(folders: Int) -> String {
    folders == 1 ? "Tidied into 1 folder" : "Tidied into \(folders) folders"
  }
}
