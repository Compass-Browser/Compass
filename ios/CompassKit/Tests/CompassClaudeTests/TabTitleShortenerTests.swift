// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Testing

@testable import CompassClaude

/// Shorter tab titles (docs/design/ai-sidebar.md, A4 on iOS), with a fake
/// Claude endpoint.
@MainActor
struct TabTitleShortenerTests {
  static let prompts = """
    ## folder_name

    Name the folder.

    ## short_titles

    Shorten the titles ({locale}). Reply {"titles": {...}}.
    """

  static let pr = "Fix login redirect by someone · Pull Request #123 · org/repo · GitHub"

  // MARK: Parsing and sanitizing

  @Test func parsesTitles() {
    let reply = ClaudeQuick.parseReply(#"{"titles": {"1": "PR #123: fix login", "2": " “Rust Book” ", "9": "Unknown"}}"#)!
    #expect(parseShortTitlesReply(reply, ids: ["1", "2", "3"]) == ["1": "PR #123: fix login", "2": "Rust Book"])
    // Another shape.
    #expect(parseShortTitlesReply(ClaudeQuick.parseReply(#"{"titles": ["A", "B"]}"#)!, ids: ["1"]).isEmpty)
    #expect(parseShortTitlesReply(ClaudeQuick.parseReply(#"{"name": "A"}"#)!, ids: ["1"]).isEmpty)
    // Not a string.
    #expect(parseShortTitlesReply(ClaudeQuick.parseReply(#"{"titles": {"1": 5, "2": null}}"#)!, ids: ["1", "2"]).isEmpty)
  }

  @Test func sanitizesTitles() {
    #expect(sanitizeShortTitle("PR #123: fix login") == "PR #123: fix login")
    #expect(sanitizeShortTitle("  \"Rust   Book\" ") == "Rust Book")
    #expect(sanitizeShortTitle("Invoice 2026-031.") == "Invoice 2026-031")
    // What a name may not hold, a title may not either (the desktop's rule).
    #expect(sanitizeShortTitle("[WIP] Sync") == nil)
    #expect(sanitizeShortTitle("東京 旅行") == "東京 旅行")
    // Too long, empty, line breaks, markup, emoji, control characters.
    #expect(sanitizeShortTitle(String(repeating: "a", count: maxShortTitleLength + 1)) == nil)
    #expect(sanitizeShortTitle(String(repeating: "a", count: maxShortTitleLength)) != nil)
    #expect(sanitizeShortTitle("  ") == nil)
    #expect(sanitizeShortTitle("Two\nlines") == nil)
    #expect(sanitizeShortTitle("<b>Bold</b>") == nil)
    #expect(sanitizeShortTitle("Flights ✈️") == nil)
    #expect(sanitizeShortTitle("A\u{202E}B") == nil)
    #expect(sanitizeShortTitle("—") == nil)
  }

  @Test func longTitles() {
    #expect(TabTitleShortener.isLong(Self.pr))
    #expect(TabTitleShortener.isLong("Never Gonna Give You Up - YouTube"))
    #expect(TabTitleShortener.isLong("The best laptops | The Verge"))
    #expect(TabTitleShortener.isLong("Forum :: Thread"))
    #expect(TabTitleShortener.isLong(String(repeating: "word ", count: 9)))
    #expect(!TabTitleShortener.isLong("Inbox"))
    #expect(!TabTitleShortener.isLong("Rust async runtimes"))
    #expect(!TabTitleShortener.isLong("- YouTube"))
  }

  @Test func theSharedPromptHasItsSection() throws {
    let url = URL(fileURLWithPath: #filePath)
      .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
      .deletingLastPathComponent().deletingLastPathComponent()
      .appendingPathComponent("common/claude/sidebar_prompts.md")
    let prompts = try String(contentsOf: url, encoding: .utf8)
    let section = SidebarPrompts.section(TabTitleShortener.promptID, in: prompts, locale: "it-IT")
    #expect(section.contains(#"{"titles": {"#))
    #expect(!section.isEmpty && !section.contains("## "))
  }

  // MARK: Asking

  @MainActor final class Setup {
    let harness = QuickHarness()
    let store = SpaceStore()
    let space: Space
    let shortener: TabTitleShortener

    init(on: Bool = true) {
      harness.on = on ? [ClaudeQuick.shortTabTitlesKey] : []
      space = store.addSpace(name: "Work")
      shortener = TabTitleShortener(
        store: store, quick: harness.quick, prompts: TabTitleShortenerTests.prompts, model: { "claude-haiku-5-5" },
        locale: { "it-IT" })
      shortener.window = .milliseconds(80)
    }

    func tab(_ title: String, _ url: String = "https://github.com/org/repo/pull/123?tab=files") -> SidebarNode {
      store.addTab(url: url, title: title, to: space.today)!
    }

    func settle() async { try? await Task.sleep(for: .milliseconds(400)) }
  }

  @Test func shortensAndCaches() async {
    let s = Setup()
    await s.harness.fake.set(reply: #"{"titles": {"1": "PR #123: fix login"}}"#)
    let tab = s.tab(Self.pr)
    s.shortener.want(tab)
    #expect(s.shortener.shortTitle(for: tab) == nil)
    await s.settle()
    #expect(await s.harness.fake.count == 1)
    #expect(s.shortener.shortTitle(for: tab) == "PR #123: fix login")
    // The page's title stays the page's: display only.
    #expect(tab.title == Self.pr && tab.displayTitle == Self.pr && tab.customTitle.isEmpty)

    // Request: the feature's model and section, titles and hosts only.
    let body = await s.harness.fake.lastBody
    #expect(body?["model"] == "claude-haiku-5-5")
    #expect(body?["system"]?.array?.last?["text"]?.string?.contains("Shorten the titles (it-IT)") == true)
    let user = body?["messages"]?.array?.first?["content"]?.array?.first?["text"]?.string ?? ""
    #expect(user.contains("1. \(Self.pr) (github.com)"))
    #expect(!user.contains("/pull/") && !user.contains("tab=files"))

    // The same (host, title) again, in another tab: from the cache.
    let again = s.tab(Self.pr, "https://github.com/org/repo/pull/123")
    s.shortener.want(again)
    await s.settle()
    #expect(await s.harness.fake.count == 1)
    #expect(s.shortener.shortTitle(for: again) == "PR #123: fix login")
    // Another site with the same title isn't the same page.
    let other = s.tab(Self.pr, "https://example.com/")
    s.shortener.want(other)
    await s.settle()
    #expect(await s.harness.fake.count == 2)
  }

  @Test func batches() async {
    let s = Setup()
    await s.harness.fake.set(reply: #"{"titles": {"1": "One", "2": "Two", "25": "Nope"}}"#)
    // Short titles aren't asked for; 25 long ones go in two requests (20
    // and 5), after the window.
    let short = s.tab("Inbox")
    s.shortener.want(short)
    var tabs: [SidebarNode] = []
    for i in 0..<25 {
      let tab = s.tab("A rather long page title, number \(i), on some site")
      tabs.append(tab)
      s.shortener.want(tab)
      s.shortener.want(tab)  // once
    }
    // 20 waiting go at once, before the window ends.
    try? await Task.sleep(for: .milliseconds(30))
    #expect(await s.harness.fake.count >= 1)
    await s.settle()
    #expect(await s.harness.fake.count == 2)
    let requests = await s.harness.fake.requests
    func user(_ request: URLRequest) -> String {
      request.httpBody.flatMap(JSONValue.parse)?["messages"]?.array?.first?["content"]?.array?.first?["text"]?.string ?? ""
    }
    let first = user(requests[0])
    let second = user(requests[1])
    #expect(first.contains("20. A rather long page title, number 19") && !first.contains("21. "))
    #expect(second.contains("5. A rather long page title, number 24") && !second.contains("6. "))
    #expect(!first.contains("Inbox"))
    // Each request's ids are its own.
    #expect(s.shortener.shortTitle(for: tabs[0]) == "One" && s.shortener.shortTitle(for: tabs[1]) == "Two")
    #expect(s.shortener.shortTitle(for: tabs[20]) == "One" && s.shortener.shortTitle(for: tabs[21]) == "Two")
    // A title without an answer keeps its own, and isn't asked again.
    #expect(s.shortener.shortTitle(for: tabs[2]) == nil)
    s.shortener.want(tabs[2])
    await s.settle()
    #expect(await s.harness.fake.count == 2)
  }

  @Test func aLongerAnswerIsDropped() async {
    let s = Setup()
    await s.harness.fake.set(reply: #"{"titles": {"1": "Never Gonna Give You Up - YouTube!"}}"#)
    let tab = s.tab("Never Gonna Give You Up - YouTube", "https://www.youtube.com/watch?v=x")
    s.shortener.want(tab)
    await s.settle()
    #expect(await s.harness.fake.count == 1)
    #expect(s.shortener.shortTitle(for: tab) == nil)
  }

  @Test func customTitleWins() async {
    let s = Setup()
    await s.harness.fake.set(reply: #"{"titles": {"1": "PR #123: fix login"}}"#)
    let tab = s.tab(Self.pr)
    s.shortener.want(tab)
    await s.settle()
    #expect(s.shortener.shortTitle(for: tab) == "PR #123: fix login")
    s.store.rename(tab, customTitle: "Login fix")
    #expect(s.shortener.shortTitle(for: tab) == nil && tab.displayTitle == "Login fix")
    // A tab named by the user isn't asked about.
    let named = s.tab("Another long title on a site somewhere · Example")
    s.store.rename(named, customTitle: "Mine")
    s.shortener.want(named)
    await s.settle()
    #expect(await s.harness.fake.count == 1)
  }

  @Test func nothingWithTheSwitchOff() async {
    let s = Setup(on: false)
    let tab = s.tab(Self.pr)
    s.shortener.want(tab)
    await s.settle()
    #expect(await s.harness.fake.count == 0)
    #expect(s.shortener.shortTitle(for: tab) == nil)

    // On, it asks; turned off again, the rows show the page's titles.
    s.harness.on = [ClaudeQuick.shortTabTitlesKey]
    await s.harness.fake.set(reply: #"{"titles": {"1": "PR #123: fix login"}}"#)
    s.shortener.want(tab)
    await s.settle()
    #expect(s.shortener.shortTitle(for: tab) == "PR #123: fix login")
    s.harness.on = []
    #expect(s.shortener.shortTitle(for: tab) == nil)
    // Not set up: nothing either.
    s.harness.on = [ClaudeQuick.shortTabTitlesKey]
    s.harness.key = nil
    s.shortener.want(s.tab("Yet another long title for a page · Example"))
    await s.settle()
    #expect(await s.harness.fake.count == 1)
  }

  @Test func neverInIncognito() async {
    let s = Setup()
    await s.harness.fake.set(reply: #"{"titles": {"1": "PR #123: fix login"}}"#)
    let tab = s.store.addTab(url: "https://github.com/org/repo/pull/123", title: Self.pr, to: s.store.incognito.today)!
    s.shortener.want(tab)
    await s.settle()
    #expect(await s.harness.fake.count == 0)
    // Even with the same page's short title known from elsewhere.
    let known = s.tab(Self.pr, "https://github.com/org/repo/pull/123")
    s.shortener.want(known)
    await s.settle()
    #expect(s.shortener.shortTitle(for: known) == "PR #123: fix login")
    #expect(s.shortener.shortTitle(for: tab) == nil)
  }

  @Test func underTheHourlyLimit() async {
    let s = Setup()
    await s.harness.fake.set(reply: #"{"titles": {}}"#)
    for i in 0..<ClaudeQuick.maxBackgroundPerHour {
      s.shortener.want(s.tab("A long title for page number \(i) on a site · Example"))
      await s.settle()
    }
    #expect(await s.harness.fake.count == ClaudeQuick.maxBackgroundPerHour)
    #expect(s.harness.quick.recentBackgroundRequests == ClaudeQuick.maxBackgroundPerHour)
    let late = s.tab("One more long title for a page · Example")
    s.shortener.want(late)
    await s.settle()
    #expect(await s.harness.fake.count == ClaudeQuick.maxBackgroundPerHour)
    // Not given up on: it's asked again when its row shows next.
    #expect(s.shortener.cache[TabTitleShortener.key(late)] == nil)
  }

  @Test func errorsAreNotCached() async {
    let s = Setup()
    await s.harness.fake.set(status: 500)
    let tab = s.tab(Self.pr)
    s.shortener.want(tab)
    await s.settle()
    #expect(await s.harness.fake.count == 1)
    #expect(s.shortener.cache[TabTitleShortener.key(tab)] == nil)
    // Nor a reply that isn't titles (the desktop's rule).
    await s.harness.fake.set(reply: "Sure!", status: 200)
    let other = s.tab("Another long title for a page · Example")
    s.harness.quick.now = { Date().addingTimeInterval(3600) }  // past the back-off
    s.shortener.want(other)
    await s.settle()
    #expect(await s.harness.fake.count == 2)
    #expect(s.shortener.cache[TabTitleShortener.key(other)] == nil)
  }

  @Test func pinnedButNotFavorites() async {
    let s = Setup()
    await s.harness.fake.set(reply: #"{"titles": {"1": "PR #123: fix login"}}"#)
    let pinned = s.store.addTab(url: "https://github.com/org/repo/pull/123", title: Self.pr, to: s.space.pinned)!
    s.shortener.want(pinned)
    await s.settle()
    #expect(s.shortener.shortTitle(for: pinned) == "PR #123: fix login")
    if let root = s.store.favoritesRoots.first,
      let favorite = s.store.addTab(url: "https://github.com/org/repo/pull/123", title: Self.pr, to: root)
    {
      #expect(s.shortener.shortTitle(for: favorite) == nil)
    }
  }
}
