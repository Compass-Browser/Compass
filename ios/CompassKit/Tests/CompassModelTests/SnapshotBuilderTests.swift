// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import Foundation
import Testing

@testable import CompassModel

@MainActor
struct SnapshotBuilderTests {
  @Test func picturesTheSidebar() {
    let store = SpaceStore()
    let work = store.addSpace(name: "Work")
    let home = store.addSpace(name: "Home")
    store.addTab(url: "https://fav.example", title: "Fav", to: store.favorites)
    store.addTab(url: "chrome://settings", to: store.favorites)
    store.addTab(url: "https://a.example", title: "A", to: work.pinned)
    let folder = store.addFolder(name: "Docs", to: work.pinned)!
    let inner = store.addFolder(name: "Inner", to: folder)!
    store.addTab(url: "https://b.example", title: "B", to: folder)
    store.addTab(url: "https://c.example", to: inner)
    let left = store.addTab(url: "https://left.example", to: work.pinned)!
    let right = store.addTab(url: "https://right.example", to: work.pinned)!
    let split = store.split(left, with: right)
    #expect(split != nil)
    store.addTab(url: "https://old.example", to: work.today)
    store.addTab(url: "https://new.example", to: work.today)

    let snapshot = store.snapshot(
      style: { _ in SpaceStyle(opacity: 0.5) }, glyph: { .initial(String($0.name.prefix(1))) },
      recentSpaceIDs: [home.id, "deleted"], syncOn: true)
    #expect(snapshot.spaces.map(\.name) == ["Work", "Home"])
    #expect(snapshot.recentSpaceIDs == [home.id])
    let w = snapshot.spaces[0]
    #expect(w.icon == .initial("W"))
    #expect(w.favorites.map(\.url) == ["https://fav.example"])
    #expect(w.pinned.map(\.url) == ["https://a.example", "https://c.example", "https://b.example", "https://left.example"])
    #expect(w.pinned.last?.id == left.id)
    #expect(w.folders == [Folder(id: folder.id, name: "Docs", children: [Folder(id: inner.id, name: "Inner")])])
    #expect(w.today.map(\.url) == ["https://new.example", "https://old.example"])
    #expect(w.todayCount == 2)
    #expect(snapshot.spaces[1].favorites.count == 1)
  }

  @Test func capsLists() {
    let store = SpaceStore()
    let space = store.addSpace(name: "S")
    for i in 0..<(Snapshot.maxEntries + 4) { store.addTab(url: "https://\(i).example", to: space.today) }
    let info = store.snapshot(style: { _ in SpaceStyle() }, glyph: { _ in .initial("S") }).spaces[0]
    #expect(info.today.count == Snapshot.maxEntries)
    #expect(info.todayCount == Snapshot.maxEntries + 4)
  }
}
