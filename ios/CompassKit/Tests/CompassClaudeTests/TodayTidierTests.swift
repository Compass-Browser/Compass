// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Testing

@testable import CompassClaude

/// Tidy Today (docs/design/ai-sidebar.md, A3), as the desktop's
/// ClaudeQuickTest.ParseTidyReply and TodayTidierBrowserTest, with a fake
/// Claude endpoint.
@MainActor
struct TodayTidierTests {
  static let prompts = """
    ## folder_name

    Name the folder.

    ## tidy_today

    Sort the tabs ({locale}). Reply {"folders": [...]}.
    """

  // MARK: The reply

  static let ids = ["1", "2", "3", "4", "5", "6", "7", "8"]

  static func parse(_ json: String, ids: [String] = ids) -> [TidyFolder] {
    parseTidyReply(ClaudeQuick.parseReply(json) ?? [:], ids: ids)
  }

  @Test func parsesFolders() {
    #expect(
      Self.parse(#"{"folders": [{"name": "Rust Async", "entries": ["1", "3"]}, {"name": "Flights", "entries": ["2", 4]}]}"#)
        == [TidyFolder(name: "Rust Async", entries: ["1", "3"]), TidyFolder(name: "Flights", entries: ["2", "4"])])
    #expect(Self.parse(#"{"folders": []}"#).isEmpty)
    // Another shape.
    #expect(Self.parse(#"{"name": "A"}"#).isEmpty)
    #expect(Self.parse(#"{"folders": {"name": "A", "entries": ["1", "2"]}}"#).isEmpty)
    #expect(Self.parse(#"{"folders": ["1", "2"]}"#).isEmpty)
  }

  @Test func dropsBadIdsAndDuplicates() {
    // Unknown ids, a fraction, and ids repeated in the folder go; the
    // folder keeps the rest.
    #expect(
      Self.parse(#"{"folders": [{"name": "A", "entries": ["1", "9", "x", 2.5, "1", "2", null, "2"]}]}"#)
        == [TidyFolder(name: "A", entries: ["1", "2"])])
    // An id an earlier folder took isn't taken again: B is left with one.
    #expect(
      Self.parse(#"{"folders": [{"name": "A", "entries": ["1", "2"]}, {"name": "B", "entries": ["2", "3"]}, {"name": "C", "entries": ["3", "4"]}]}"#)
        == [TidyFolder(name: "A", entries: ["1", "2"]), TidyFolder(name: "C", entries: ["3", "4"])])
    // A folder dropped for its name takes nothing.
    #expect(
      Self.parse(#"{"folders": [{"name": "Way Too Many Words Here", "entries": ["1", "2"]}, {"name": "B", "entries": ["1", "2"]}]}"#)
        == [TidyFolder(name: "B", entries: ["1", "2"])])
  }

  @Test func dropsSingletonsAndBadNames() {
    #expect(Self.parse(#"{"folders": [{"name": "A", "entries": ["1"]}, {"name": "B", "entries": []}]}"#).isEmpty)
    #expect(Self.parse(#"{"folders": [{"name": "", "entries": ["1", "2"]}]}"#).isEmpty)
    #expect(Self.parse(#"{"folders": [{"name": "Flights ✈️", "entries": ["1", "2"]}]}"#).isEmpty)
    #expect(Self.parse(#"{"folders": [{"entries": ["1", "2"]}, {"name": "A"}]}"#).isEmpty)
    // Names are trimmed as SanitizeAiName() does.
    #expect(Self.parse(#"{"folders": [{"name": " \"Rust Async.\" ", "entries": ["1", "2"]}]}"#).first?.name == "Rust Async")
  }

  @Test func atMostSixFolders() {
    let ids = (1...20).map(String.init)
    let folders = (0..<8).map { i in #"{"name": "F\#(i)", "entries": ["\#(2 * i + 1)", "\#(2 * i + 2)"]}"# }
    let parsed = Self.parse(#"{"folders": [\#(folders.joined(separator: ", "))]}"#, ids: ids)
    #expect(parsed.count == maxTidyFolders && parsed.map(\.name) == ["F0", "F1", "F2", "F3", "F4", "F5"])
    // Folders that don't pass don't count towards the six.
    let some = [#"{"name": "X", "entries": ["1"]}"#] + folders.dropFirst()
    #expect(Self.parse(#"{"folders": [\#(some.joined(separator: ", "))]}"#, ids: ids).first?.name == "F1")
  }

  @Test func theSharedPromptHasItsSection() throws {
    // common/claude/sidebar_prompts.md, as the app bundles it.
    let url = URL(fileURLWithPath: #filePath)
      .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
      .deletingLastPathComponent().deletingLastPathComponent()
      .appendingPathComponent("common/claude/sidebar_prompts.md")
    let prompts = try String(contentsOf: url, encoding: .utf8)
    let section = SidebarPrompts.section(TodayTidier.promptID, in: prompts, locale: "it-IT")
    #expect(section.contains(#"{"folders": [{"name": "<name>", "entries": ["<id>", "<id>"]}]}"#))
    #expect(section.contains("(it-IT)") && section.contains("at most 6 folders"))
    #expect(!section.contains("## "))
  }

  // MARK: Tidying

  @MainActor final class Setup {
    let harness = QuickHarness()
    let store = SpaceStore()
    let space: Space
    let tidier: TodayTidier
    var tabs: [SidebarNode] = []

    /// Today with `count` tabs, the first added at the bottom.
    init(tabs count: Int = 6) {
      harness.on = [ClaudeQuick.tidyTodayKey]
      space = store.addSpace(name: "Work")
      tidier = TodayTidier(
        store: store, quick: harness.quick, prompts: TodayTidierTests.prompts, model: { "claude-haiku-5-5" },
        locale: { "it-IT" })
      for i in 0..<count {
        tabs.append(store.addTab(url: "https://site\(i).example/path?q=\(i)", title: "Tab \(i)", to: space.today)!)
      }
    }

    var today: [String] { space.today.children.map(\.id) }
  }

  @Test func tidiesAndUndoes() async {
    let s = Setup()
    let t = s.tabs
    // Listed from Today's top: t5 is 1, t0 is 6.
    await s.harness.fake.set(
      reply: #"{"folders": [{"name": "Odd Ones", "entries": ["2", "4", "6"]}, {"name": "Pair", "entries": ["1", "3"]}]}"#)
    let before = s.today
    let outcome = await s.tidier.tidy(s.space)
    guard case .tidied(let undo, let folders) = outcome else {
      Issue.record("not tidied: \(String(describing: outcome))")
      return
    }
    #expect(folders == 2 && TodayTidier.text(folders: folders) == "Tidied into 2 folders")
    #expect(await s.harness.fake.count == 1)

    // What was asked: the section, the locale, the model, titles and hosts.
    let body = await s.harness.fake.lastBody
    let user = body?["messages"]?.array?.first?["content"]?.array?.first?["text"]?.string ?? ""
    #expect(user == """
      Today's loose entries:
      1. Tab 5 (site5.example)
      2. Tab 4 (site4.example)
      3. Tab 3 (site3.example)
      4. Tab 2 (site2.example)
      5. Tab 1 (site1.example)
      6. Tab 0 (site0.example)

      """)
    #expect(body?["system"]?.array?.last?["text"]?.string == #"Sort the tabs (it-IT). Reply {"folders": [...]}."#)
    #expect(body?["model"] == "claude-haiku-5-5" && body?["max_tokens"]?.number == 400)

    // Folders where their topmost entry was, entries topmost first,
    // named by Claude.
    // t1, in no group, stays loose.
    let odd = s.store.node(id: s.today[1])!
    let pair = s.store.node(id: s.today[2])!
    #expect(s.today.count == 3 && s.today[0] == t[1].id)
    #expect(odd.title == "Odd Ones" && odd.nameSource == .claude && odd.children.map(\.id) == [t[4].id, t[2].id, t[0].id])
    #expect(pair.title == "Pair" && pair.nameSource == .claude && pair.children.map(\.id) == [t[5].id, t[3].id])

    #expect(s.tidier.canUndo(undo))
    #expect(s.tidier.undo(undo))
    #expect(s.today == before && s.store.node(id: odd.id) == nil && s.store.node(id: pair.id) == nil)
    #expect(!s.tidier.canUndo(undo) && !s.tidier.undo(undo))
  }

  @Test func splitsAsOneAndFoldersLeftAlone() async {
    let s = Setup()
    let t = s.tabs
    let split = s.store.split(t[1], with: t[2])!
    let mine = s.store.addFolder(name: "Mine", to: s.space.today)!
    let inside = s.store.addTab(url: "https://in.example/", title: "Inside", to: mine)!
    // Loose: t5, t4, t3, the split, t0 (the folder isn't).
    #expect(s.tidier.looseEntries(of: s.space).map(\.id) == [t[5].id, t[4].id, t[3].id, split.id, t[0].id])
    let user = TodayTidier.describe(s.tidier.looseEntries(of: s.space))
    #expect(user.contains("4. Tab 1 + Tab 2 (site1.example, site2.example)\n"))
    #expect(!user.contains("Inside") && !user.contains("Mine"))

    await s.harness.fake.set(reply: #"{"folders": [{"name": "Panes", "entries": ["4", "5"]}]}"#)
    guard case .tidied = await s.tidier.tidy(s.space) else {
      Issue.record("not tidied")
      return
    }
    let panes = split.parent!
    #expect(panes.title == "Panes" && panes.children.map(\.id) == [split.id, t[0].id])
    #expect(split.children.map(\.id) == [t[1].id, t[2].id])
    #expect(mine.children.map(\.id) == [inside.id] && mine.title == "Mine")
  }

  @Test func nothingIsSentWithTheSwitchOff() async {
    let s = Setup()
    s.harness.on = [ClaudeQuick.nameLinkFoldersKey]
    #expect(!s.tidier.isAvailable(in: s.space) && !s.tidier.canTidy(s.space))
    #expect(await s.tidier.tidy(s.space) == nil)
    // Nor without a key.
    s.harness.on = [ClaudeQuick.tidyTodayKey]
    s.harness.key = nil
    #expect(!s.tidier.isAvailable(in: s.space))
    #expect(await s.tidier.tidy(s.space) == nil)
    #expect(await s.harness.fake.count == 0)
    #expect(s.space.today.children.allSatisfy { $0.isTab })
  }

  @Test func turnedOffWhileClaudeAnswersChangesNothing() async {
    let s = Setup()
    await s.harness.fake.set(reply: #"{"folders": [{"name": "Pair", "entries": ["1", "2"]}]}"#, delay: .milliseconds(200))
    let before = s.today
    let tidying = Task { await s.tidier.tidy(s.space) }
    try? await Task.sleep(for: .milliseconds(50))
    #expect(s.tidier.isBusy(s.space) && !s.tidier.canTidy(s.space))
    s.harness.on = []
    #expect(await tidying.value == nil)
    #expect(!s.tidier.isBusy(s.space) && s.today == before)
  }

  @Test func neverInIncognitoNorWithFewEntries() async {
    let s = Setup(tabs: 4)
    #expect(s.tidier.isAvailable(in: s.space) && !s.tidier.canTidy(s.space))
    #expect(await s.tidier.tidy(s.space) == nil)
    for i in 0..<6 {
      s.store.addTab(url: "https://private\(i).example/", title: "Private \(i)", to: s.store.incognito.today)
    }
    #expect(!s.tidier.isAvailable(in: s.store.incognito) && !s.tidier.canTidy(s.store.incognito))
    #expect(await s.tidier.tidy(s.store.incognito) == nil)
    #expect(await s.harness.fake.count == 0)
  }

  @Test func onlyShownEntriesAreTidied() async {
    let s = Setup()
    let tidier = TodayTidier(
      store: s.store, quick: s.harness.quick, prompts: Self.prompts, model: { "" },
      isShown: { !$0.url.contains("site0") })
    #expect(tidier.looseEntries(of: s.space).count == 5 && tidier.canTidy(s.space))
    s.store.remove(s.tabs[1])
    #expect(!tidier.canTidy(s.space))
  }

  @Test func errorsSayWhy() async {
    let s = Setup()
    let before = s.today
    await s.harness.fake.set(reply: #"{"folders": [{"name": "Lonely", "entries": ["1"]}]}"#)
    #expect(await s.tidier.tidy(s.space) == .failed("The AI found nothing to group."))
    await s.harness.fake.set(reply: "Sure! Here are your folders.")
    #expect(await s.tidier.tidy(s.space) == .failed("Couldn't tidy Today."))
    await s.harness.fake.set(status: 500)
    #expect(await s.tidier.tidy(s.space) == .failed("Couldn't tidy Today: the AI couldn't be reached."))
    s.harness.quick.timeout = 0.05
    await s.harness.fake.set(status: 200, delay: .milliseconds(500))
    #expect(await s.tidier.tidy(s.space) == .failed("Couldn't tidy Today: the AI took too long."))
    #expect(s.today == before)
  }

  @Test func bypassesTheHourlyLimit() async {
    let s = Setup()
    // The hour's background requests used up, and backing off after an
    // error: Tidy is the user's request, so it still asks.
    s.harness.on.insert(ClaudeQuick.nameLinkFoldersKey)
    for _ in 0..<ClaudeQuick.maxBackgroundPerHour { _ = await s.harness.quick.ask(s.harness.request()) }
    #expect(await s.harness.quick.ask(s.harness.request()) == .failure(.rateLimited))
    await s.harness.fake.set(reply: #"{"folders": [{"name": "Pair", "entries": ["1", "2"]}]}"#)
    guard case .tidied = await s.tidier.tidy(s.space) else {
      Issue.record("not tidied")
      return
    }
  }
}
