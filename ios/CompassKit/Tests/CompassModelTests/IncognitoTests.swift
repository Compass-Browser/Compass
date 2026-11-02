// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassModel

/// The incognito space (docs/design/ios.md, "Incognito"): found by lookups,
/// never reported to sync, never saved, sealed off from the other spaces.
@MainActor
struct IncognitoTests {
  @Test func notInSpacesButFound() throws {
    let store = SpaceStore()
    let home = store.addSpace(name: "Home")
    let tab = try #require(store.addTab(url: "https://secret.example", to: store.incognito.today))
    #expect(store.spaces.map(\.id) == [home.id])
    #expect(store.node(id: tab.id) === tab)
    #expect(store.space(of: tab) === store.incognito)
    #expect(store.space(id: "incognito") === store.incognito)
    #expect(store.isIncognito(tab))
    #expect(store.incognito.theme.appearance == .dark)
  }

  @Test func changesNeverReachSync() throws {
    let store = SpaceStore()
    let home = store.addSpace(name: "Home")
    var changes: [SpaceStoreChange] = []
    store.onLocalChange = { changes.append($0) }
    let tab = try #require(store.addTab(url: "https://secret.example", to: store.incognito.today))
    store.update(tab, url: "https://secret.example/2", title: "Secret")
    let folder = try #require(store.addFolder(name: "F", to: store.incognito.today))
    store.batch {
      let brief = store.addTab(url: "https://brief.example", to: store.incognito.today)!
      store.remove(brief)
    }
    store.remove(folder)
    store.archive(tab)
    #expect(changes.isEmpty)
    // Normal changes still go.
    _ = store.addTab(url: "https://open.example", to: home.today)
    #expect(changes.count == 1)
    #expect(!changes[0].putNodes.isEmpty)
  }

  @Test func archivingCloses() throws {
    let store = SpaceStore()
    let tab = try #require(store.addTab(url: "https://a.example", to: store.incognito.today))
    store.archive(tab)
    #expect(store.incognito.today.children.isEmpty)
    #expect(store.incognito.archive.children.isEmpty)
  }

  @Test func sealedOff() throws {
    let store = SpaceStore()
    let home = store.addSpace(name: "Home")
    let secret = try #require(store.addTab(url: "https://secret.example", to: store.incognito.today))
    let open = try #require(store.addTab(url: "https://open.example", to: home.today))
    #expect(!store.move(secret, to: home.today, at: 0))
    #expect(!store.move(secret, to: store.favorites, at: 0))
    #expect(!store.move(open, to: store.incognito.today, at: 0))
    #expect(store.isIncognito(secret))
    #expect(!store.isIncognito(open))
  }

  @Test func neverSaved() throws {
    let store = SpaceStore()
    _ = store.addSpace(name: "Home")
    _ = store.addTab(url: "https://secret.example", to: store.incognito.today)
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("incognito-\(UUID()).json")
    try store.save(to: url)
    let text = try String(contentsOf: url, encoding: .utf8)
    try? FileManager.default.removeItem(at: url)
    #expect(!text.contains("secret.example"))
    #expect(!text.contains("Incognito"))
  }
}
