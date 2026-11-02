// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassModel
import Foundation
import Testing

@MainActor
struct SpaceStoreTests {
  let store = SpaceStore()
  var changes: [SpaceStoreChange] = []

  func make() -> (SpaceStore, Space, () -> [SpaceStoreChange]) {
    let store = SpaceStore()
    var log: [SpaceStoreChange] = []
    store.onLocalChange = { log.append($0) }
    let space = store.addSpace(name: "Work")
    log.removeAll()
    return (store, space, { log })
  }

  @Test func addKeepsOrder() {
    let (store, space, _) = make()
    let a = store.addTab(url: "https://a", to: space.pinned)!
    let c = store.addTab(url: "https://c", to: space.pinned)!
    let b = store.addTab(url: "https://b", to: space.pinned, at: 1)!
    #expect(space.pinned.children.map(\.id) == [a.id, b.id, c.id])
    #expect(a.position < b.position && b.position < c.position)
    let first = store.addTab(url: "https://0", to: space.pinned, at: 0)!
    #expect(space.pinned.children.first === first)
  }

  @Test func foldersInPinnedAndToday() {
    let (store, space, _) = make()
    #expect(store.addFolder(name: "T", to: space.today) != nil)
    #expect(store.addFolder(name: "F", to: store.favorites) == nil)
    #expect(store.addFolder(name: "A", to: space.archive) == nil)
    let folder = store.addFolder(name: "F", to: space.pinned)!
    let inner = store.addFolder(name: "G", to: folder)!
    #expect(inner.parent === folder)
    #expect(store.canMove(folder, to: space.today))
    #expect(!store.canMove(folder, to: space.archive))
    #expect(!store.canMove(folder, to: inner))  // cycle
    #expect(!store.canMove(space.pinned, to: folder))  // roots don't move
  }

  @Test func favoritesHoldOnlyTabs() {
    let (store, space, _) = make()
    let tab = store.addTab(url: "https://f", to: store.favorites)
    #expect(tab != nil)
    let other = store.addTab(url: "https://x", to: space.today)!
    let pane = store.addTab(url: "https://y", to: space.today)!
    let split = store.split(other, with: pane)!
    #expect(!store.canMove(split, to: store.favorites))
  }

  @Test func splitsAndNormalization() {
    let (store, space, log) = make()
    let a = store.addTab(url: "https://a", to: space.today)!
    let b = store.addTab(url: "https://b", to: space.today)!
    let c = store.addTab(url: "https://c", to: space.today)!
    let split = store.split(a, with: b)!
    #expect(split.children.map(\.id) == [a.id, b.id])
    #expect(space.today.children.count == 2)
    #expect(store.split(split, with: c) == nil)  // at most two panes
    #expect(!store.canMove(c, to: split))

    // Moving a pane out leaves one: the split is replaced by it.
    store.move(b, to: space.today, at: 0)
    #expect(a.parent === space.today)
    #expect(store.node(id: split.id) == nil)
    #expect(log().contains { $0.deletedNodes.contains(split.id) })
  }

  @Test func emptiedTodayFoldersClose() {
    let (store, space, log) = make()
    let trip = store.addFolder(name: "Trip", to: space.today)!
    let hotels = store.addFolder(name: "Hotels", to: trip)!
    let hotel = store.addTab(url: "https://hotel", to: hotels)!
    let flight = store.addTab(url: "https://flight", to: trip)!
    let later = store.addFolder(name: "Later", to: space.today)!
    // Made empty, a folder stays.
    #expect(space.today.children.map(\.id) == [trip.id, later.id])

    // Its last child gone, it closes, and its emptied parents after it.
    store.remove(flight)
    #expect(trip.children.map(\.id) == [hotels.id])
    store.move(hotel, to: space.today, at: 0)
    #expect(space.today.children.map(\.id) == [hotel.id, later.id])
    #expect(log().reduce(into: Set<String>()) { $0.formUnion($1.deletedNodes) }.isSuperset(of: [hotels.id, trip.id]))

    // Pinned, the same.
    let a = store.addTab(url: "https://a", to: later)!
    store.move(a, to: space.pinned, at: 0)
    #expect(space.today.children.map(\.id) == [hotel.id])

    // A split's last pane leaving takes its folder too.
    let inbox = store.addFolder(name: "Inbox", to: space.today)!
    let left = store.addTab(url: "https://l", to: inbox)!
    let split = store.split(left, with: store.addTab(url: "https://r", to: inbox)!)!
    store.remove(split)
    #expect(store.node(id: inbox.id) == nil)

    // Pinned folders stay.
    let work = store.addFolder(name: "Work", to: space.pinned)!
    store.remove(store.addTab(url: "https://s", to: work)!)
    #expect(work.parent === space.pinned)

    // Not while applying remote changes.
    let synced = store.addFolder(name: "Synced", to: space.today)!
    let b = store.addTab(url: "https://b", to: synced)!
    store.applyingRemote { store.remove(b) }
    #expect(synced.parent === space.today)
  }

  @Test func removeReportsTheSubtree() {
    let (store, space, log) = make()
    let folder = store.addFolder(name: "F", to: space.pinned)!
    let tab = store.addTab(url: "https://a", to: folder)!
    store.remove(folder)
    #expect(log().last?.deletedNodes == [folder.id, tab.id])
    #expect(space.pinned.children.isEmpty)
  }

  @Test func sectionMovesReportTheSubtree() {
    let (store, space, log) = make()
    let a = store.addTab(url: "https://a", to: space.today)!
    let b = store.addTab(url: "https://b", to: space.today)!
    let split = store.split(a, with: b)!
    store.move(split, to: space.pinned, at: 0)
    #expect(log().last?.putNodes == [split.id, a.id, b.id])
  }

  @Test func todayChurnIsSeparate() {
    let (store, space, log) = make()
    let a = store.addTab(url: "https://a", to: space.today)!
    store.update(a, url: "https://a/2", title: "A2")
    #expect(log().last?.todayChurn == [a.id])
    #expect(log().last?.putNodes.isEmpty == true)
    let p = store.addTab(url: "https://p", to: space.pinned)!
    store.update(p, title: "P")
    #expect(log().last?.putNodes == [p.id])
  }

  @Test func archiveIdleEntries() {
    let (store, space, _) = make()
    let now = Date()
    store.setArchiveAfter(space, 3600)
    let old = store.addTab(url: "https://old", to: space.today, now: now.addingTimeInterval(-7200))!
    let fresh = store.addTab(url: "https://new", to: space.today, now: now)!
    #expect(store.archiveIdleEntries(now: now) == 1)
    #expect(old.parent === space.archive)
    #expect(old.archivedAt == now)
    #expect(fresh.parent === space.today)

    // Restoring brings it back to Today under a fresh ID.
    let restored = store.restore(old, now: now)!
    #expect(restored.id != old.id && restored.url == "https://old")
    #expect(restored.parent === space.today && restored.lastActive == now)
    #expect(store.node(id: old.id) == nil)

    store.archive(restored, now: now.addingTimeInterval(-31 * 86400))
    store.pruneArchive(now: now)
    #expect(store.node(id: restored.id) == nil)
  }

  @Test func remoteChangesAreNotReported() {
    let (store, space, log) = make()
    store.applyingRemote {
      let n = store.makeNode(
        id: "x", kind: .tab, url: "https://x", position: .initial(suffix: UniquePosition.randomSuffix()))
      store.rawInsert(n, into: space.pinned)
    }
    #expect(log().isEmpty)
    #expect(store.node(id: "x") != nil)
  }

  @Test func spacesKeepTheirOrder() {
    let (store, work, _) = make()
    let home = store.addSpace(name: "Home")
    let play = store.addSpace(name: "Play")
    #expect(store.spaces.map(\.name) == ["Work", "Home", "Play"])
    store.moveSpace(play, to: 0)
    #expect(store.spaces.map(\.name) == ["Play", "Work", "Home"])
    #expect(play.position < work.position && work.position < home.position)
    store.deleteSpace(work)
    #expect(store.spaces.map(\.name) == ["Play", "Home"])
  }

  @Test func snapshotRoundTrips() throws {
    let (store, space, _) = make()
    let folder = store.addFolder(name: "F", to: space.pinned)!
    store.addTab(url: "https://a", title: "A", to: folder)
    let t1 = store.addTab(url: "https://b", to: space.today)!
    let t2 = store.addTab(url: "https://c", to: space.today)!
    store.split(t1, with: t2)
    store.addTab(url: "https://fav", to: store.favorites)
    store.setCollapsed(folder, true)

    let copy = SpaceStore()
    try copy.load(snapshot: try store.encodedSnapshot())
    #expect(try copy.encodedSnapshot() == store.encodedSnapshot())
    let loaded = copy.space(id: space.id)!
    #expect(loaded.pinned.children.first?.collapsed == true)
    #expect(loaded.today.children.first?.isSplit == true)
    #expect(copy.favorites.children.map(\.url) == ["https://fav"])
  }

  @Test func batchGroupsChanges() {
    let (store, space, log) = make()
    store.batch {
      store.addTab(url: "https://a", to: space.today)
      store.addTab(url: "https://b", to: space.today)
    }
    #expect(log().count == 1)
    #expect(log()[0].putNodes.count == 2)
  }
}
