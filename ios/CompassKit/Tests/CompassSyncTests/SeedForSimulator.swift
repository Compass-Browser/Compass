// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
@testable import CompassSync
import Foundation
import Testing

/// Not a test: plays "the Mac" for a simulator run. With
/// COMPASS_SEED_SERVER=<url> (a running server) it starts the chain of the
/// fixed seed below and fills a sidebar; then launch the app with
/// -CompassServer <url> -CompassJoinRecovery "abandon amount liar amount
/// expire adjust cage candy arch gather drum buyer".
@Suite(.enabled(if: ProcessInfo.processInfo.environment["COMPASS_SEED_SERVER"] != nil))
@MainActor
struct SeedForSimulator {
  @Test func seed() async throws {
    let server = URL(string: ProcessInfo.processInfo.environment["COMPASS_SEED_SERVER"]!)!
    let chain = try Chain(seed: Data(hex: "000102030405060708090a0b0c0d0e0f")!)
    let mac = await SidebarDevice(server: server, chain: chain, first: true)
    try await mac.sync()
    let store = mac.store
    let work = store.addSpace(name: "Work", color: .purple)
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
    let home = store.addSpace(name: "Personal", color: .green)
    store.addTab(url: "https://www.bbc.com/news", title: "BBC News", to: home.pinned)
    store.addTab(url: "https://www.nytimes.com/", title: "NYTimes", to: home.today)
    try await mac.sync()
  }
}
