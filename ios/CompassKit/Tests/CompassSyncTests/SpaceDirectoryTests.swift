// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
@testable import CompassSync
import Foundation
import Testing

@MainActor
struct SpaceDirectoryTests {
  let work = "2b5c3e1a-0000-4000-8000-00000000000b"

  @Test func pairingListSkipsBadEntries() {
    let list = ChainProfile.list(json: [
      ["uuid": "2b5c3e1a-0000-4000-8000-00000000000a", "name": "Home", "home": true],
      ["uuid": "2B5C3E1A-0000-4000-8000-00000000000B", "name": "Upper case"],
      ["uuid": work, "name": "Work", "avatar": 27],
      ["uuid": work, "name": "Repeat"],
      ["name": "No UUID"],
    ])
    #expect(list.map(\.name) == ["Home", "Work"])
    #expect(list[1].avatar == 27)
    #expect(ChainProfile.list(json: nil).isEmpty)
  }

  /// Two devices that each started as the home: the smallest UUID wins, as
  /// on the desktop.
  @Test func smallestHomeWins() {
    let list = [
      ChainProfile(uuid: "b0000000-0000-4000-8000-000000000000", name: "B", home: true),
      ChainProfile(uuid: "a0000000-0000-4000-8000-000000000000", name: "A", home: true),
      ChainProfile(uuid: work, name: "Work"),
    ]
    #expect(ChainProfile.homeUUID(list) == "a0000000-0000-4000-8000-000000000000")
    #expect(ChainProfile.sorted(list).map(\.name) == ["A", "Work"])
  }

  /// The command bar offers only the current space's profile: its spaces'
  /// tabs and favorites, and its account's history.
  @Test func commandBarIsScopedToTheProfile() throws {
    let store = SpaceStore()
    store.homeProfileUUID = "2b5c3e1a-0000-4000-8000-00000000000a"
    let personal = store.addSpace(name: "Personal")
    let side = store.addSpace(name: "Side", profile: store.homeProfileUUID)
    let job = store.addSpace(name: "Job", profile: work)
    store.addTab(url: "https://p.example/", title: "P", to: personal.today)
    store.addTab(url: "https://s.example/", title: "S", to: side.pinned)
    store.addTab(url: "https://hf.example/", title: "HF", to: store.favorites(for: side))
    store.addTab(url: "https://j.example/", title: "J", to: job.today)
    store.addTab(url: "https://jf.example/", title: "JF", to: store.favorites(for: job))
    #expect(Set(store.tabs(inProfileOf: personal).map(\.title)) == ["P", "S", "HF"])
    #expect(Set(store.tabs(inProfileOf: job).map(\.title)) == ["J", "JF"])
    #expect(store.spaces(inProfileOf: side).map(\.name) == ["Personal", "Side"])

    let history = try HistoryStore()
    history.add(Visit(time: 1, url: "https://p.example/a", title: "Shared word", originator: "x"))
    history.add(Visit(time: 2, url: "https://j.example/a", title: "Shared word", originator: "x", profile: work))
    #expect(history.search("shared", profile: "").map(\.url) == ["https://p.example/a"])
    #expect(history.search("shared", profile: work).map(\.url) == ["https://j.example/a"])
    #expect(history.recent(profile: work).map(\.url) == ["https://j.example/a"])
    #expect(history.search("shared").count == 2)
  }

  @Test func spacesAndFavoritesGoToTheirProfilesBridge() {
    let store = SpaceStore()
    let directory = SpaceDirectory(store: store)
    let engine = SyncEngine(connection: NoConnection(), passphrase: "p", store: MemorySyncMetadataStore())
    let home = CompassSpacesBridge(directory: directory, profile: "", engine: engine, mirrorURL: nil)
    let workBridge = CompassSpacesBridge(directory: directory, profile: work, engine: engine, mirrorURL: nil)
    _ = home.mergeWithChain()
    _ = workBridge.mergeWithChain()

    let personal = store.addSpace(name: "Personal")
    let job = store.addSpace(name: "Job", profile: work)
    let tab = store.addTab(url: "https://a.example/", to: job.pinned)!
    let fav = store.addTab(url: "https://f.example/", to: store.favorites(for: job))!
    #expect(directory.owner(of: personal) === home)
    #expect(directory.owner(of: job) === workBridge)
    #expect(directory.owner(of: fav) === workBridge)
    #expect(workBridge.mirror[tab.id] != nil && home.mirror[tab.id] == nil)
    #expect(workBridge.mirror[fav.id] != nil && home.mirror[fav.id] == nil)

    // A tab moved to a space of the other profile changes accounts.
    store.move(tab, toSpace: personal, section: .pinned)
    #expect(home.mirror[tab.id] != nil && workBridge.mirror[tab.id] == nil)

    // A whole space too, with its entries.
    let other = store.addTab(url: "https://b.example/", to: job.today)!
    store.setProfile(job, to: "")
    #expect(home.mirror[job.id] != nil && home.mirror[other.id] != nil)
    #expect(workBridge.mirror[job.id] == nil && workBridge.mirror[other.id] == nil)
    #expect(home.mirror[job.id]?.space.profileUuid == "")
  }
}
