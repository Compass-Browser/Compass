// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassModel

/// Link folders (docs/design/sidebar.md, "Link folders") and where a
/// folder's name came from (docs/design/ai-sidebar.md), as the desktop's
/// TabStripBridgeTest.LinksGatherWithTheirOpenerInAFolder and
/// SpaceModelTest's NameSource tests.
@MainActor
struct LinkFolderTests {
  @Test func linksGatherWithTheirOpenerInAFolder() {
    let store = SpaceStore()
    let space = store.addSpace(name: "Work")
    let older = store.addTab(url: "https://older.example/", title: "Older", to: space.today)!
    let opener = store.addTab(url: "https://docs.example/a", title: "Docs", to: space.today)!
    let newer = store.addTab(url: "https://newer.example/", title: "Newer", to: space.today)!
    var links = LinkFolders()

    let first = links.addLink(url: "https://docs.example/1", from: opener, in: space, store: store)!
    // A folder named after the opener took its place, holding it and then
    // the link.
    let folder = first.parent!
    #expect(folder.isFolder && folder.title == "Docs" && folder.parent === space.today)
    #expect(space.today.children.map(\.id) == [older.id, folder.id, newer.id])
    #expect(folder.children.map(\.id) == [opener.id, first.id])
    #expect(folder.nameSource == .opener && folder.linkFolderHere)
    #expect(first.lastActive != nil)

    // The next link goes after the one before, in the same folder.
    let second = links.addLink(url: "https://docs.example/2", from: opener, in: space, store: store)!
    #expect(folder.children.map(\.id) == [opener.id, first.id, second.id])

    // A link from another entry of the folder goes right after it.
    let third = links.addLink(url: "https://docs.example/3", from: first, in: space, store: store)!
    #expect(third.parent === folder)
    #expect(folder.children.map(\.id) == [opener.id, first.id, third.id, second.id])
    #expect(space.today.children.filter(\.isFolder).count == 1)
  }

  @Test func linksJoinTheOpenersFolder() {
    let store = SpaceStore()
    let space = store.addSpace(name: "Work")
    let mine = store.addFolder(name: "Mine", to: space.today)!
    let opener = store.addTab(url: "https://a.example/", title: "A", to: mine)!
    var links = LinkFolders()
    let link = links.addLink(url: "https://b.example/", from: opener, in: space, store: store)!
    #expect(link.parent === mine)
    #expect(mine.children.map(\.id) == [opener.id, link.id])
    // A folder made by hand stays the user's.
    #expect(mine.nameSource == .user && !mine.linkFolderHere)
  }

  @Test func aSplitOpenerGoesInWhole() {
    let store = SpaceStore()
    let space = store.addSpace(name: "Work")
    let a = store.addTab(url: "https://a.example/", title: "A", to: space.today)!
    let b = store.addTab(url: "https://b.example/", title: "B", to: space.today)!
    let split = store.split(a, with: b)!
    var links = LinkFolders()
    let link = links.addLink(url: "https://c.example/", from: b, in: space, store: store)!
    let folder = link.parent!
    #expect(folder.children.map(\.id) == [split.id, link.id])
    #expect(folder.title == LinkFolders.title(of: split))
  }

  @Test func linksFromPinnedEntriesGatherAtTheTopOfToday() {
    let store = SpaceStore()
    let space = store.addSpace(name: "Work")
    let pinned = store.addTab(url: "https://mail.example/", to: space.pinned)!
    let today = store.addTab(url: "https://t.example/", title: "T", to: space.today)!
    var links = LinkFolders()
    let first = links.addLink(url: "https://mail.example/1", from: pinned, in: space, store: store)!
    let folder = first.parent!
    // The pinned entry stays put; its folder is the newest Today entry,
    // named after its host (it has no title).
    #expect(pinned.parent === space.pinned)
    #expect(space.today.children.map(\.id) == [today.id, folder.id])
    #expect(folder.title == "mail.example" && folder.nameSource == .opener)
    let second = links.addLink(url: "https://mail.example/2", from: pinned, in: space, store: store)!
    #expect(folder.children.map(\.id) == [first.id, second.id])

    // A favorite's links, likewise; once its folder is gone, a new one.
    let favorite = store.addTab(url: "https://news.example/", title: "News", to: store.favorites)!
    var link = links.addLink(url: "https://news.example/1", from: favorite, in: space, store: store)!
    let newsFolder = link.parent!
    #expect(newsFolder.title == "News" && newsFolder !== folder)
    store.remove(newsFolder)
    link = links.addLink(url: "https://news.example/2", from: favorite, in: space, store: store)!
    #expect(link.parent !== newsFolder && link.parent?.title == "News")
  }

  @Test func incognitoLinksStayIncognito() {
    let store = SpaceStore()
    let opener = store.addTab(url: "https://a.example/", title: "A", to: store.incognito.today)!
    var links = LinkFolders()
    let link = links.addLink(url: "https://b.example/", from: opener, in: store.incognito, store: store)!
    #expect(store.isIncognito(link) && link.parent?.isFolder == true)
  }

  // MARK: NameSource

  @Test func aUserRenameIsTheUsers() {
    let store = SpaceStore()
    let space = store.addSpace(name: "Work")
    let folder = store.addFolder(name: "Docs", to: space.today)!
    #expect(folder.nameSource == .user)
    store.markLinkFolder(folder)
    #expect(folder.nameSource == .opener && folder.linkFolderHere)

    store.setFolderName(folder, "Rust Async", source: .claude)
    #expect(folder.title == "Rust Async" && folder.nameSource == .claude && folder.nameBeforeClaude == "Docs")
    // Claude's names follow one another: Undo Name goes back to the first.
    store.setFolderName(folder, "Tokio", source: .claude)
    #expect(folder.nameBeforeClaude == "Docs")
    #expect(store.undoFolderName(folder))
    #expect(folder.title == "Docs" && folder.nameSource == .user && folder.nameBeforeClaude.isEmpty)
    #expect(!store.undoFolderName(folder))

    // Renaming by hand only counts when the name changes.
    let other = store.addFolder(name: "Links", to: space.today)!
    store.markLinkFolder(other)
    store.rename(other, customTitle: "Links")
    #expect(other.nameSource == .opener)
    store.rename(other, customTitle: "Reading")
    #expect(other.nameSource == .user)
  }

  @Test func nameChangesAreReported() {
    let store = SpaceStore()
    let space = store.addSpace(name: "Work")
    let folder = store.addFolder(name: "Docs", to: space.today)!
    var changes: [SpaceStoreChange] = []
    store.onLocalChange = { changes.append($0) }
    var observed = 0
    store.localChangeObservers.append { _ in observed += 1 }
    store.markLinkFolder(folder)
    store.setFolderName(folder, "Rust", source: .claude)
    #expect(changes.count == 2 && changes.allSatisfy { $0.putNodes == [folder.id] })
    #expect(observed == 2)
  }

  @Test func roundTripsNameSources() throws {
    let store = SpaceStore()
    let space = store.addSpace(name: "Work")
    let link = store.addFolder(name: "Docs", to: space.today)!
    store.markLinkFolder(link)
    let named = store.addFolder(name: "Docs 2", to: space.today)!
    store.markLinkFolder(named)
    store.setFolderName(named, "Rust Async", source: .claude)
    let mine = store.addFolder(name: "Mine", to: space.pinned)!

    let loaded = SpaceStore()
    try loaded.load(snapshot: try store.encodedSnapshot())
    #expect(loaded.node(id: link.id)?.nameSource == .opener)
    #expect(loaded.node(id: named.id)?.nameSource == .claude)
    #expect(loaded.node(id: named.id)?.nameBeforeClaude == "Docs 2")
    #expect(loaded.node(id: mine.id)?.nameSource == .user)
    // Made on this device in an earlier run: Claude doesn't name it again.
    #expect(loaded.node(id: link.id)?.linkFolderHere == false)

    // A file from before: every folder is the user's.
    let text = String(decoding: try store.encodedSnapshot(), as: UTF8.self)
      .replacingOccurrences(of: "\"nameSource\":\"opener\",", with: "")
      .replacingOccurrences(of: "\"nameSource\":\"claude\",", with: "")
    let old = SpaceStore()
    try old.load(snapshot: Data(text.utf8))
    #expect(old.node(id: link.id)?.nameSource == .user && old.node(id: named.id)?.nameSource == .user)
  }
}
