// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
@testable import CompassSync
import Foundation
import Testing

/// A device with a real sidebar: SpaceStore + CompassSpacesBridge + engine.
@MainActor
struct SidebarDevice {
  let store = SpaceStore()
  let engine: SyncEngine
  let bridge: CompassSpacesBridge

  init(server: URL, chain: Chain, first: Bool) async {
    let metadata = SyncMetadata()
    engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server, cacheGUID: metadata.cacheGUID, authKey: chain.authKey),
      passphrase: chain.passphrase, store: MemorySyncMetadataStore(metadata), createChainIfEmpty: first)
    bridge = CompassSpacesBridge(store: store, engine: engine, mirrorURL: nil)
    await engine.register(bridge)
  }

  func sync() async throws {
    await bridge.flush()
    try await engine.sync()
    await bridge.flush()
    if await engine.hasPendingChanges { try await engine.sync() }
  }

  /// The sidebar as text, for comparing two devices.
  var outline: String {
    func line(_ n: SidebarNode, _ depth: Int) -> [String] {
      let kind: String =
        switch n.kind {
        case .folder: "folder"
        case .split: "split"
        default: "tab"
        }
      return [String(repeating: "  ", count: depth) + "\(kind) \(n.displayTitle) \(n.url)"]
        + n.children.flatMap { line($0, depth + 1) }
    }
    var out = ["favorites"] + store.favorites.children.flatMap { line($0, 1) }
    for s in store.spaces {
      out.append("space \(s.name) \(s.theme.palette.map { "\($0)" } ?? "custom")")
      for root in [s.pinned, s.today, s.archive] {
        out.append(" \(root.id.split(separator: ":")[0])")
        out += root.children.flatMap { line($0, 2) }
      }
    }
    return out.joined(separator: "\n")
  }
}

@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
@MainActor
struct SpacesServerTests {
  let chain = try! Chain(seed: Chain.generateSeed())

  @Test func sidebarRoundTrip() async throws {
    let server = try await TestServer()
    let mac = await SidebarDevice(server: server.url, chain: chain, first: true)
    try await mac.sync()

    let work = mac.store.addSpace(name: "Work", theme: SpaceTheme(.green))
    let docs = mac.store.addFolder(name: "Docs", to: work.pinned)!
    mac.store.addTab(url: "https://docs.example/", title: "Docs home", to: docs)
    let inner = mac.store.addFolder(name: "Inner", to: docs)!
    mac.store.addTab(url: "https://deep.example/", title: "Deep", to: inner)
    let a = mac.store.addTab(url: "https://a.example/", title: "A", to: work.today)!
    let b = mac.store.addTab(url: "https://b.example/", title: "B", to: work.today)!
    mac.store.split(a, with: b)
    mac.store.addTab(url: "https://fav.example/", title: "Fav", to: mac.store.favorites)
    let play = mac.store.addSpace(name: "Play", theme: SpaceTheme(.pink))
    mac.store.addTab(url: "https://play.example/", title: "Play", to: play.pinned)
    try await mac.sync()

    let phone = await SidebarDevice(server: server.url, chain: chain, first: false)
    try await phone.sync()
    #expect(phone.outline == mac.outline, "\n\(phone.outline)\n---\n\(mac.outline)")

    // The phone reorders, renames and deletes; the Mac follows.
    let phoneWork = phone.store.spaces.first { $0.name == "Work" }!
    phone.store.moveSpace(phone.store.spaces.last!, to: 0)
    phone.store.rename(phoneWork.pinned.children[0], customTitle: "Documents")
    let deep = phone.store.allNodes.first { $0.url == "https://deep.example/" }!
    phone.store.move(deep, to: phoneWork.pinned, at: 0)
    phone.store.remove(phone.store.allNodes.first { $0.displayTitle == "Inner" }!)
    phone.store.addTab(url: "https://phone.example/", title: "From phone", to: phoneWork.today)
    try await phone.sync()
    try await mac.sync()
    #expect(mac.outline == phone.outline, "\n\(mac.outline)\n---\n\(phone.outline)")
    #expect(mac.store.spaces.map(\.name) == ["Play", "Work"])

    // Nothing left to commit on either side: no echo ping-pong.
    try await mac.sync()
    try await phone.sync()
    #expect(await mac.engine.hasPendingChanges == false)
    #expect(await phone.engine.hasPendingChanges == false)
  }

  @Test func archiveMovesAcrossDevices() async throws {
    let server = try await TestServer()
    let mac = await SidebarDevice(server: server.url, chain: chain, first: true)
    try await mac.sync()
    let space = mac.store.addSpace(name: "Work")
    let old = mac.store.addTab(url: "https://old.example/", title: "Old", to: space.today)!
    try await mac.sync()
    let phone = await SidebarDevice(server: server.url, chain: chain, first: false)
    try await phone.sync()

    mac.store.archive(old)
    try await mac.sync()
    try await phone.sync()
    let phoneSpace = phone.store.spaces[0]
    #expect(phoneSpace.today.children.isEmpty)
    #expect(phoneSpace.archive.children.map(\.url) == ["https://old.example/"])

    // Restoring on the phone: fresh IDs, back in Today on the Mac.
    phone.store.restore(phoneSpace.archive.children[0])
    try await phone.sync()
    try await mac.sync()
    #expect(space.archive.children.isEmpty)
    #expect(space.today.children.map(\.url) == ["https://old.example/"])
    #expect(space.today.children[0].id != old.id)
  }
}
