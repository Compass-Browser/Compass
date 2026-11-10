// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassModel
import Foundation
import Testing

/// SpaceStore's live folder rules, as the desktop's SpaceModelTest.Live*.
@MainActor
struct LiveFolderModelTests {
  func make() -> (SpaceStore, Space, SidebarNode, () -> [SpaceStoreChange]) {
    let store = SpaceStore()
    var log: [SpaceStoreChange] = []
    store.onLocalChange = { log.append($0) }
    let space = store.addSpace(name: "Work")
    let folder = store.addLiveFolder(name: "News", source: LiveSource(pluginID: "compass.rss"), to: space.pinned)!
    log.removeAll()
    return (store, space, folder, { log })
  }

  func items(_ keys: String...) -> [LiveItem] {
    keys.map { LiveItem(key: $0, url: "https://example.com/\($0)", title: "Item \($0)") }
  }

  @Test func onlyInPinned() {
    let store = SpaceStore()
    let space = store.addSpace(name: "W")
    let source = LiveSource(pluginID: "compass.rss")
    #expect(store.addLiveFolder(name: "T", source: source, to: space.today) == nil)
    #expect(store.addLiveFolder(name: "F", source: source, to: store.favorites) == nil)
    #expect(store.addLiveFolder(name: "I", source: source, to: store.incognito.pinned) == nil)
    #expect(store.addLiveFolder(name: "N", source: LiveSource(pluginID: ""), to: space.pinned) == nil)
    let plain = store.addFolder(name: "P", to: space.pinned)!
    let live = store.addLiveFolder(name: "L", source: source, to: plain)!
    #expect(live.isLiveFolder && live.parent === plain)
    // Not in another live folder, and not out of Pinned.
    #expect(store.addLiveFolder(name: "X", source: source, to: live) == nil)
    #expect(!store.canMove(live, to: space.today))
    #expect(!store.canMove(plain, to: space.today))  // it holds one
    #expect(store.canMove(live, to: space.pinned))
    let other = store.addSpace(name: "Other")
    #expect(store.move(live, toSpace: other, section: .pinned))
    #expect(!store.move(live, toSpace: other, section: .today))
  }

  @Test func holdsOnlyItsEntries() {
    let (store, space, folder, _) = make()
    #expect(store.addTab(url: "https://x", to: folder) == nil)
    #expect(store.addFolder(name: "F", to: folder) == nil)
    let tab = store.addTab(url: "https://y", to: space.pinned)!
    #expect(!store.canMove(tab, to: folder))
    store.replaceLiveItems(folder, items: items("a", "b"))
    let a = folder.children[0]
    // Not reordered, not split.
    #expect(!store.canMove(a, to: folder))
    #expect(store.split(a, with: tab) == nil)
  }

  @Test func replaceDiffsByKey() {
    let (store, _, folder, log) = make()
    store.replaceLiveItems(folder, items: items("a", "b", "c"), now: Date(timeIntervalSince1970: 1))
    #expect(folder.children.map(\.liveItemKey) == ["a", "b", "c"])
    #expect(folder.children.map(\.url) == ["https://example.com/a", "https://example.com/b", "https://example.com/c"])
    #expect(folder.children.allSatisfy { !$0.unseen })  // first refresh: nothing new
    #expect(folder.children.allSatisfy { $0.savedURL == $0.url })
    #expect(folder.liveRefreshed == Date(timeIntervalSince1970: 1))
    #expect(folder.unseenCount == 0)
    let (a, b, c) = (folder.children[0], folder.children[1], folder.children[2])
    #expect(a.position < b.position && b.position < c.position)

    // b goes, d comes first, c moves before a, a's title changes; repeats
    // and empty keys are skipped.
    var next = items("d", "c", "a", "c")
    next.append(LiveItem(key: "", url: "https://example.com/e", title: "E"))
    next[2].title = "A2"
    store.replaceLiveItems(folder, items: next, now: Date(timeIntervalSince1970: 2))
    #expect(folder.children.map(\.liveItemKey) == ["d", "c", "a"])
    #expect(folder.children[1] === c && folder.children[2] === a)  // same nodes
    #expect(a.title == "A2" && a.savedTitle == "A2")
    #expect(folder.children[0].unseen && !a.unseen && folder.unseenCount == 1)
    #expect(store.node(id: b.id) == nil)
    #expect(zip(folder.children, folder.children.dropFirst()).allSatisfy { $0.position < $1.position })
    #expect(log().contains { $0.deletedNodes.contains(b.id) })

    store.markAllSeen(folder)
    #expect(folder.unseenCount == 0)
  }

  @Test func openEntriesStay() {
    let (store, _, folder, _) = make()
    store.replaceLiveItems(folder, items: items("a", "b"))
    let (a, b) = (folder.children[0], folder.children[1])
    store.update(a, url: "https://elsewhere.example/", title: "Elsewhere")
    var next = items("a")
    next[0].title = "New A"
    store.replaceLiveItems(folder, items: next) { $0 === a || $0 === b }
    // a keeps its page but takes the item as its pinned URL; b stays, last.
    #expect(a.url == "https://elsewhere.example/" && a.savedTitle == "New A")
    #expect(folder.children.map(\.id) == [a.id, b.id])
    store.replaceLiveItems(folder, items: next)
    #expect(folder.children.map(\.id) == [a.id])
  }

  @Test func movedOutBecomesAnOrdinaryTab() {
    let (store, space, folder, _) = make()
    store.replaceLiveItems(folder, items: items("a"))
    store.replaceLiveItems(folder, items: items("a", "b"))
    let b = folder.children[1]
    #expect(b.unseen)
    #expect(store.move(b, to: space.pinned, at: 0))
    #expect(!b.isLiveItem && !b.unseen)
    // And it doesn't come back: a new entry takes its key.
    store.replaceLiveItems(folder, items: items("a", "b"))
    #expect(folder.children.count == 2 && folder.children[1] !== b)
  }

  @Test func sourceAndConversion() {
    let (store, _, folder, log) = make()
    store.replaceLiveItems(folder, items: items("a", "b"))
    store.replaceLiveItems(folder, items: items("a", "b", "c"))
    let changed = LiveSource(pluginID: "compass.rss", configJSON: #"{"url":"https://x.example/feed"}"#)
    store.setLiveSource(folder, changed)
    #expect(folder.liveSource == changed)
    #expect(log().last?.putNodes == [folder.id])

    store.convertToPlainFolder(folder)
    #expect(!folder.isLiveFolder && folder.liveRefreshed == nil)
    #expect(folder.children.allSatisfy { !$0.isLiveItem && !$0.unseen })
    #expect(log().last?.putNodes == Set(folder.subtree.map(\.id)))
    #expect(store.liveSourceCleared(folder))
    // A plain folder now: anything goes in.
    #expect(store.addTab(url: "https://x", to: folder) != nil)
  }

  @Test func savedAndLoaded() throws {
    let (store, space, folder, _) = make()
    store.setLiveSource(folder, LiveSource(pluginID: "com.example.feed", configJSON: #"{"n":1}"#,
                                           pluginURL: "https://example.com/p/manifest.json", pluginSHA256: "ab"))
    store.replaceLiveItems(folder, items: items("a"), now: Date(timeIntervalSince1970: 100))
    store.replaceLiveItems(folder, items: items("a", "b"), now: Date(timeIntervalSince1970: 200))
    let data = try store.encodedSnapshot()

    let loaded = SpaceStore()
    try loaded.load(snapshot: data)
    let f = loaded.node(id: folder.id)!
    #expect(f.liveSource == folder.liveSource)
    #expect(f.liveRefreshed == Date(timeIntervalSince1970: 200))
    #expect(f.children.map(\.liveItemKey) == ["a", "b"])
    #expect(f.children.map(\.unseen) == [false, true])
    #expect(loaded.space(id: space.id)!.pinned.children.first === f)
  }
}
