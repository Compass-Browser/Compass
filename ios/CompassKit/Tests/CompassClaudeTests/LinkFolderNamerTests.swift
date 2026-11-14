// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Testing

@testable import CompassClaude

/// Names for link folders (docs/design/ai-sidebar.md, A2), as the desktop's
/// LinkFolderNamerBrowserTest, with a fake Claude endpoint.
@MainActor
struct LinkFolderNamerTests {
  static let prompts = """
    ## folder_name

    Name the folder ({locale}). Reply {"name": "<the name>"}.
    """

  @MainActor final class Setup {
    let harness = QuickHarness()
    let store = SpaceStore()
    let space: Space
    let namer: LinkFolderNamer
    var links = LinkFolders()

    init() {
      space = store.addSpace(name: "Work")
      namer = LinkFolderNamer(
        store: store, quick: harness.quick, prompts: LinkFolderNamerTests.prompts, model: { "claude-haiku-5-5" },
        locale: { "it-IT" })
      namer.debounce = .milliseconds(50)
    }

    /// A Today page and `count` links opened from it: its link folder.
    func linkFolder(links count: Int) -> SidebarNode {
      let opener = store.addTab(url: "https://docs.rs/tokio", title: "Tokio", to: space.today)!
      var folder: SidebarNode!
      for i in 0..<count {
        folder = links.addLink(url: "https://docs.rs/tokio/\(i)", from: opener, in: space, store: store)!.parent
      }
      return folder
    }
  }

  /// Lets the debounce and the request run.
  func settle() async { try? await Task.sleep(for: .milliseconds(400)) }

  @Test func namesALinkFolderAtThreeEntries() async {
    let s = Setup()
    let folder = s.linkFolder(links: 1)  // the opener and a link: 2
    await settle()
    #expect(await s.harness.fake.count == 0)
    _ = s.links.addLink(url: "https://docs.rs/tokio/x", from: folder.children[0], in: s.space, store: s.store)
    await settle()
    #expect(await s.harness.fake.count == 1)
    #expect(folder.title == "Rust Async" && folder.nameSource == .claude && folder.nameBeforeClaude == "Tokio")
    #expect(s.namer.sparkling.contains(folder.id))
    #expect(s.namer.canUndo(folder) && !s.namer.canSuggest(folder))

    // What was asked: titles and hosts, the prompt with the locale.
    let body = await s.harness.fake.lastBody
    let user = body?["messages"]?.array?.first?["content"]?.array?.first?["text"]?.string ?? ""
    #expect(user.hasPrefix("The folder's tabs:\n- Tokio (docs.rs)\n"))
    #expect(!user.contains("/tokio/"))
    #expect(body?["system"]?.array?.last?["text"]?.string?.contains("(it-IT)") == true)

    // Not again until 6 entries.
    _ = s.links.addLink(url: "https://docs.rs/tokio/y", from: folder.children[0], in: s.space, store: s.store)
    await settle()
    #expect(await s.harness.fake.count == 1)
    for i in 0..<2 {
      _ = s.links.addLink(url: "https://docs.rs/tokio/z\(i)", from: folder.children[0], in: s.space, store: s.store)
    }
    await s.harness.fake.set(reply: #"{"name": "Tokio Runtime"}"#)
    await settle()
    #expect(await s.harness.fake.count == 2)
    #expect(folder.title == "Tokio Runtime" && folder.nameBeforeClaude == "Tokio")

    // Undo Name: the opener's name back, as the user's; never named again.
    #expect(s.store.undoFolderName(folder))
    #expect(folder.title == "Tokio" && folder.nameSource == .user)
    for i in 0..<6 {
      _ = s.links.addLink(url: "https://docs.rs/tokio/w\(i)", from: folder.children[0], in: s.space, store: s.store)
    }
    await settle()
    #expect(await s.harness.fake.count == 2)
  }

  @Test func aUserRenameStopsNaming() async {
    let s = Setup()
    let folder = s.linkFolder(links: 1)
    s.store.rename(folder, customTitle: "My Reading")
    _ = s.links.addLink(url: "https://docs.rs/tokio/x", from: folder.children[0], in: s.space, store: s.store)
    await settle()
    #expect(await s.harness.fake.count == 0)
    #expect(folder.title == "My Reading")
  }

  @Test func aRenameWhileClaudeThinksWins() async {
    let s = Setup()
    await s.harness.fake.set(delay: .milliseconds(300))
    let folder = s.linkFolder(links: 2)
    try? await Task.sleep(for: .milliseconds(150))  // asked, not answered
    s.store.rename(folder, customTitle: "Mine")
    await settle()
    #expect(await s.harness.fake.count == 1)
    #expect(folder.title == "Mine" && folder.nameSource == .user)
  }

  @Test func noRequestWithTheSwitchOff() async {
    let s = Setup()
    s.harness.on = []
    let folder = s.linkFolder(links: 5)
    await settle()
    #expect(!s.namer.canSuggest(folder))
    s.namer.suggest(folder)
    await settle()
    #expect(await s.harness.fake.count == 0)
    #expect(folder.nameSource == .opener)
  }

  @Test func neverInIncognito() async {
    let s = Setup()
    let opener = s.store.addTab(url: "https://a.example/", title: "A", to: s.store.incognito.today)!
    var folder: SidebarNode?
    for i in 0..<3 {
      folder = s.links.addLink(url: "https://a.example/\(i)", from: opener, in: s.store.incognito, store: s.store)?.parent
    }
    await settle()
    #expect(await s.harness.fake.count == 0)
    #expect(folder.map { s.namer.canSuggest($0) } == false)
  }

  @Test func onlyThisDevicesLinkFolders() async {
    let s = Setup()
    // A folder made by hand, or a link folder from another device (synced:
    // NameSource opener, not made here).
    let mine = s.store.addFolder(name: "Mine", to: s.space.today)!
    let synced = s.store.addFolder(name: "Synced", to: s.space.today)!
    s.store.rawSetNameSource(synced, .opener)
    for folder in [mine, synced] {
      for i in 0..<3 { s.store.addTab(url: "https://b.example/\(i)", title: "B\(i)", to: folder) }
    }
    await settle()
    #expect(await s.harness.fake.count == 0)

    // Suggest a Name works on any folder with entries.
    #expect(s.namer.canSuggest(mine))
    s.namer.suggest(mine)
    await settle()
    #expect(await s.harness.fake.count == 1)
    #expect(mine.title == "Rust Async" && mine.nameSource == .claude && mine.nameBeforeClaude == "Mine")
    let empty = s.store.addFolder(name: "Empty", to: s.space.pinned)!
    #expect(!s.namer.canSuggest(empty))
  }

  @Test func badNamesAreDropped() async {
    let s = Setup()
    await s.harness.fake.set(reply: #"{"name": "Tabs 🦀 Rust"}"#)
    let folder = s.linkFolder(links: 2)
    await settle()
    #expect(await s.harness.fake.count == 1)
    #expect(folder.title == "Tokio" && folder.nameSource == .opener)
  }

  @Test func describesTitlesAndHosts() {
    let store = SpaceStore()
    let space = store.addSpace(name: "W")
    let folder = store.addFolder(name: "F", to: space.today)!
    store.addTab(url: "https://a.example/path?q=secret", title: "Line\nbreak", to: folder)
    let named = store.addTab(url: "https://b.example/", title: "Page", to: folder)!
    store.rename(named, customTitle: "Mine")
    store.addTab(url: "https://c.example/", to: folder)
    store.addTab(url: "", to: folder)
    store.addTab(url: "https://d.example/", title: String(repeating: "x", count: 200), to: folder)
    let text = LinkFolderNamer.describe(folder)
    #expect(
      text == "The folder's tabs:\n- Line break (a.example)\n- Mine (b.example)\n- c.example\n- "
        + String(repeating: "x", count: 120) + "\u{2026} (d.example)\n")
  }
}
