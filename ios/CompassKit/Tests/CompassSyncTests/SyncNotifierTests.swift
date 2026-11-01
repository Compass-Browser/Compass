// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import Testing

struct EventStreamParserTests {
  func parse(_ text: String) -> [EventStreamParser.Event] {
    var p = EventStreamParser()
    return Array(text.utf8).flatMap { p.feed($0) }
  }

  @Test func parsesEventsWithAnyLineEnds() {
    let lf = parse(": connected\n\nevent: changed\ndata: {\"types\":[970000]}\n\n")
    #expect(lf == [.init(name: "changed", data: "{\"types\":[970000]}")])
    let crlf = parse("event: changed\r\ndata: a\r\ndata: b\r\n\r\n")
    #expect(crlf == [.init(name: "changed", data: "a\nb")])
    let cr = parse("event:changed\rdata:x\r\r: keep-alive\r\r")
    #expect(cr == [.init(name: "changed", data: "x")])
  }

  @Test func typesFromData() {
    #expect(SyncNotifier.types("{\"types\":[45873,970000,1]}") == [.passwords, .compassSpaces])
    #expect(SyncNotifier.types("nonsense") == nil)
  }
}

actor Heard {
  var events: [[DataType]?] = []
  func add(_ e: [DataType]?) { events.append(e) }
}

@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
struct SyncNotifierServerTests {
  @Test func hearsAnotherDevicesCommit() async throws {
    let chain = try Chain(seed: Chain.generateSeed())
    let server = try await TestServer()
    let mac = await TestDevice(server: server.url, chain: chain, name: "Mac", first: true)
    try await mac.sync()
    let phoneGUID = "phone-guid"
    let notifier = SyncNotifier(serverURL: server.url, cacheGUID: phoneGUID, authKey: chain.authKey)
    let heard = Heard()
    let task = Task { await notifier.run { await heard.add($0) } }
    defer { task.cancel() }
    // Connected: a refresh of everything first.
    for _ in 0..<100 where await heard.events.isEmpty { try await Task.sleep(for: .milliseconds(20)) }
    #expect(await heard.events.first! == nil)

    await mac.spaces.set("s", "Work", engine: mac.engine)
    try await mac.sync()
    for _ in 0..<100 where await heard.events.count < 2 { try await Task.sleep(for: .milliseconds(20)) }
    let events = await heard.events
    #expect(events.count >= 2)
    #expect(events.last??.contains(.compassSpaces) == true)
  }
}
