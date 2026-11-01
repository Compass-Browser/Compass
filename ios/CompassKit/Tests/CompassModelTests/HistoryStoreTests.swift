// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Testing

struct HistoryStoreTests {
  func visit(_ seconds: TimeInterval, _ url: String, _ title: String, _ device: String = "phone") -> Visit {
    Visit(time: Visit.time(Date(timeIntervalSince1970: 1_700_000_000 + seconds)), url: url, title: title, originator: device)
  }

  @Test func searchesWordsInURLAndTitle() throws {
    let h = try HistoryStore()
    h.add(visit(0, "https://swift.org/documentation/", "Swift Documentation"))
    h.add(visit(10, "https://github.com/apple/swift", "apple/swift: The Swift Programming Language"))
    h.add(visit(20, "https://news.ycombinator.com/", "Hacker News"))
    h.add(visit(30, "https://swift.org/documentation/", "Swift Documentation", "mac"))
    #expect(h.search("swift").map(\.url) == ["https://swift.org/documentation/", "https://github.com/apple/swift"])
    #expect(h.search("swift").first?.visitCount == 2)
    #expect(h.search("hack").map(\.title) == ["Hacker News"])
    #expect(h.search("ycombinator").count == 1)
    #expect(h.search("nothing here").isEmpty)
  }

  @Test func sameDeviceAndTimeIsOneVisit() throws {
    let h = try HistoryStore()
    h.add(visit(0, "https://a/", "Loading…"))
    h.add(visit(0, "https://a/", "A"))
    #expect(h.count == 1)
    #expect(h.search("a").first?.title == "A")
    h.add(visit(0, "https://a/", "A", "mac"))
    #expect(h.count == 2)
  }

  @Test func deletes() throws {
    let h = try HistoryStore()
    for i in 0..<10 { h.add(visit(TimeInterval(i), "https://p\(i % 3)/", "P")) }
    h.delete(from: visit(2, "", "").time, to: visit(4, "", "").time)
    #expect(h.count == 7)
    h.delete(url: "https://p0/", until: visit(100, "", "").time)
    #expect(h.recent().map(\.url).contains("https://p0/") == false)
    h.deleteAll()
    #expect(h.count == 0)
  }

  @Test func timeIsWindowsEpochMicros() {
    #expect(Visit.time(Date(timeIntervalSince1970: 0)) == 11_644_473_600_000_000)
    let v = visit(0.25, "", "")
    #expect(v.date == Date(timeIntervalSince1970: 1_700_000_000.25))
  }
}
