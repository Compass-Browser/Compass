// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
@testable import CompassSync
import Foundation
import SyncProto
import Testing

/// Not a test: plays "the Mac" for a simulator run. With
/// COMPASS_SEED_SERVER=<url> (a running server) it starts the chain of the
/// fixed seed below and fills a sidebar; then launch the app with
/// -CompassServer <url> -CompassJoinRecovery "abandon amount liar amount
/// expire adjust cage candy arch gather drum buyer".
/// With COMPASS_SEED_PROFILES=1 the chain has two profiles (M8): Home with
/// "Personal" and "Side", Work (its own account) with "Work", in the order
/// Personal, Work, Side.
@Suite(.enabled(if: ProcessInfo.processInfo.environment["COMPASS_SEED_SERVER"] != nil))
@MainActor
struct SeedForSimulator {
  /// COMPASS_SEED_SEND_TAB=<url>: "the Mac" sends that page to the joined
  /// phone (docs/design/send-tab.md), which should show a notification.
  private func sendTab(_ url: String, server: URL, chain: Chain) async throws {
    let metadata = SyncMetadata()
    let engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server, cacheGUID: metadata.cacheGUID, authKey: chain.authKey),
      passphrase: chain.passphrase, store: MemorySyncMetadataStore(metadata), createChainIfEmpty: false)
    let devices = DeviceInfoBridge(local: .init(name: "MacBook", model: "Mac", appVersion: "1"))
    await engine.register(devices)
    // The engine commits only registered types.
    await engine.register(SendTabBridge(engine: engine, cacheGUID: metadata.cacheGUID, stateURL: nil))
    try await engine.sync()
    // The test "Macs" use the phone's DeviceInfoBridge too: every other
    // device gets one.
    let targets = devices.allDevices.filter { !$0.isThisDevice }
    try #require(!targets.isEmpty, "no other device in the chain")
    for phone in targets {
    var entry = SyncPb_SendTabToSelfSpecifics()
    entry.guid = UUID().uuidString
    entry.url = url
    entry.title = "Sent from the Mac"
    entry.deviceName = "MacBook"
    entry.targetDeviceSyncCacheGuid = phone.cacheGUID
    entry.sharedTimeUsec = Int64(Date().timeIntervalSince1970 * 1_000_000)
    var specifics = SyncPb_EntitySpecifics()
    specifics.sendTabToSelf = entry
    await engine.put(.sendTabToSelf, tag: entry.guid, specifics: specifics)
    }
    try await engine.sync()
  }

  @Test func seed() async throws {
    let server = URL(string: ProcessInfo.processInfo.environment["COMPASS_SEED_SERVER"]!)!
    let chain = try Chain(seed: Data(hex: "000102030405060708090a0b0c0d0e0f")!)
    if ProcessInfo.processInfo.environment["COMPASS_SEED_PROFILES"] != nil {
      try await seedProfiles(server: server, chain: chain)
      return
    }
    if let url = ProcessInfo.processInfo.environment["COMPASS_SEED_SEND_TAB"] {
      try await sendTab(url, server: server, chain: chain)
      return
    }
    let mac = await SidebarDevice(server: server, chain: chain, first: true)
    try await mac.sync()
    let store = mac.store
    if ProcessInfo.processInfo.environment["COMPASS_SEED_PUSH"] != nil {
      // A later run while the app is open: one more space, which should
      // appear through push without touching the app.
      store.addSpace(name: "Pushed", theme: SpaceTheme(.orange))
      try await mac.sync()
      return
    }
    let work = store.addSpace(name: "Work", theme: SpaceTheme(.purple))
    let docs = store.addFolder(name: "Projects", to: work.pinned)!
    store.addTab(url: "https://github.com/", title: "GitHub", to: docs)
    store.addTab(url: "https://developer.apple.com/documentation/swiftui", title: "SwiftUI", to: docs)
    store.addTab(url: "https://www.figma.com/", title: "Figma", to: work.pinned)
    store.addTab(url: "https://linear.app/", title: "Linear", to: work.pinned)
    let a = store.addTab(url: "https://news.ycombinator.com/", title: "Hacker News", to: work.today)!
    let b = store.addTab(url: "https://lobste.rs/", title: "Lobsters", to: work.today)!
    store.split(a, with: b)
    store.addTab(url: "https://en.wikipedia.org/wiki/Compass", title: "Compass - Wikipedia", to: work.today)
    for (url, title) in [
      ("https://mail.google.com/", "Mail"), ("https://calendar.google.com/", "Calendar"),
      ("https://www.youtube.com/", "YouTube"), ("https://www.apple.com/", "Apple"),
    ] {
      store.addTab(url: url, title: title, to: store.favorites)
    }
    let home = store.addSpace(name: "Personal", theme: SpaceTheme(.green))
    store.addTab(url: "https://www.bbc.com/news", title: "BBC News", to: home.pinned)
    store.addTab(url: "https://www.nytimes.com/", title: "NYTimes", to: home.today)
    try await mac.sync()
  }

  func seedProfiles(server: URL, chain: Chain) async throws {
    let home = "5eed0000-0000-4000-8000-00000000000a"
    let work = "5eed0000-0000-4000-8000-00000000000b"
    let mac = await ProfilesDevice(server: server, chain: chain, first: true)
    await mac.addAccount(work)
    mac.store.homeProfileUUID = home
    try await mac.sync()
    await mac.publishProfiles([
      ChainProfile(uuid: home, name: "Personal", home: true), ChainProfile(uuid: work, name: "Work"),
    ])
    let store = mac.store
    let personal = store.addSpace(name: "Personal", theme: SpaceTheme(.green), profile: home)
    store.addTab(url: "https://www.bbc.com/news", title: "BBC News", to: personal.pinned)
    store.addTab(url: "https://www.nytimes.com/", title: "NYTimes", to: personal.today)
    let w = store.addSpace(name: "Work", theme: SpaceTheme(.purple), profile: work)
    let docs = store.addFolder(name: "Projects", to: w.pinned)!
    store.addTab(url: "https://github.com/", title: "GitHub", to: docs)
    store.addTab(url: "https://linear.app/", title: "Linear", to: w.pinned)
    store.addTab(url: "https://www.figma.com/", title: "Figma", to: w.pinned)
    store.addTab(url: "https://news.ycombinator.com/", title: "Hacker News", to: w.today)
    let side = store.addSpace(name: "Side", theme: SpaceTheme(.orange), profile: home)
    store.addTab(url: "https://en.wikipedia.org/wiki/Compass", title: "Compass - Wikipedia", to: side.pinned)
    for (url, title) in [("https://mail.google.com/", "Mail"), ("https://www.youtube.com/", "YouTube")] {
      store.addTab(url: url, title: title, to: store.favorites(forProfile: home))
    }
    for (url, title) in [
      ("https://calendar.google.com/", "Calendar"), ("https://slack.com/", "Slack"), ("https://www.notion.so/", "Notion"),
    ] {
      store.addTab(url: url, title: title, to: store.favorites(forProfile: work))
    }
    try await mac.syncUntilQuiet()
  }
}
