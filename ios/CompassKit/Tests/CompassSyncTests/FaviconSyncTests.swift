// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
@testable import CompassSync
import Foundation
import Testing

/// docs/design/sync.md, "Favicons".
struct FaviconKeyTests {
  /// The desktop's unit test checks the same vectors.
  @Test func uuidFromHost() {
    #expect(CompassSpacesBridge.faviconUUID(host: "www.example.com") == "c94c25bc-f583-5f0a-88eb-63e59b454926")
    #expect(CompassSpacesBridge.faviconUUID(host: "github.com") == "b73d689b-d088-5dec-9f35-e41fb59757a0")
    let id = CompassSpacesBridge.faviconUUID(host: "www.example.com")
    #expect(CompassSpacesBridge.isValidUUID(id))
    #expect(Array(id)[14] == "5")
    #expect("89ab".contains(Array(id)[19]))
    #expect(CompassSpacesBridge.faviconUUID(host: "WWW.Example.com") == id)
    #expect(CompassSpacesBridge.faviconUUID(host: "example.com") != id)
  }
}

@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
@MainActor
struct FaviconServerTests {
  let chain = try! Chain(seed: Chain.generateSeed())
  let small = Data(repeating: 1, count: 300)
  let large = Data(repeating: 2, count: 900)

  @Test func iconsTravelAndTheLargerWins() async throws {
    let server = try await TestServer()
    let mac = await SidebarDevice(server: server.url, chain: chain, first: true)
    try await mac.sync()
    let space = mac.store.addSpace(name: "Work", color: .blue)
    let tab = mac.store.addTab(url: "https://www.example.com/a", title: "A", to: space.pinned)!
    try await mac.sync()

    let phone = await SidebarDevice(server: server.url, chain: chain, first: false)
    var received: [String: Data] = [:]
    phone.bridge.onFavicon = { received[$0] = $1 }
    try await phone.sync()

    // Only hosts an entry points at.
    #expect(!mac.bridge.offerFavicon(host: "other.example", png: small, sourceSize: 32))
    #expect(mac.bridge.offerFavicon(host: "www.example.com", png: small, sourceSize: 32))
    try await mac.sync()
    try await phone.sync()
    #expect(received["www.example.com"] == small)
    #expect(phone.bridge.syncedFavicons.map(\.host) == ["www.example.com"])

    // The same size and a fresh icon: kept. Larger: replaces it.
    #expect(!phone.bridge.offerFavicon(host: "www.example.com", png: large, sourceSize: 32))
    #expect(phone.bridge.offerFavicon(host: "www.example.com", png: large, sourceSize: 180))
    var macReceived: [String: Data] = [:]
    mac.bridge.onFavicon = { macReceived[$0] = $1 }
    try await phone.sync()
    try await mac.sync()
    #expect(macReceived["www.example.com"] == large)
    #expect(!mac.bridge.offerFavicon(host: "www.example.com", png: small, sourceSize: 32))

    // A week later, a different icon of the same size replaces it.
    mac.bridge.now = { Date().addingTimeInterval(8 * 24 * 60 * 60) }
    #expect(mac.bridge.offerFavicon(host: "www.example.com", png: Data(repeating: 3, count: 900), sourceSize: 180))
    try await mac.sync()

    // No entry on the host any more: gone once past the grace period.
    mac.store.remove(tab)
    try await mac.sync()
    #expect(mac.bridge.syncedFavicons.count == 1)  // written "now" (8 days on): not a day old yet
    mac.bridge.now = { Date().addingTimeInterval(10 * 24 * 60 * 60) }
    mac.bridge.collectFavicons()
    try await mac.sync()
    #expect(mac.bridge.syncedFavicons.isEmpty)
    try await phone.sync()
    #expect(phone.bridge.syncedFavicons.isEmpty)
    // The sidebar is untouched by icons.
    #expect(phone.outline == mac.outline)
  }
}
