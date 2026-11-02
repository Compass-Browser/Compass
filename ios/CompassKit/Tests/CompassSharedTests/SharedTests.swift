// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassShared

struct DeepLinkTests {
  @Test(arguments: [
    DeepLink.open(url: "https://example.com/a?b=1&c=2#x", space: "s1"),
    .open(url: "https://example.com/", space: nil),
    .node("0b7e-1"),
    .space("abc"),
    .search,
    .newTab(space: nil),
    .newTab(space: "s2"),
    .inbox,
    .downloads,
  ])
  func roundTrips(_ link: DeepLink) {
    #expect(DeepLink(url: link.url) == link)
  }

  @Test func keepsTheTargetsQuery() {
    let link = DeepLink.open(url: "https://example.com/?q=a+b&x=1", space: nil)
    #expect(link.url.absoluteString == "compass://open?url=https://example.com/%3Fq%3Da%2Bb%26x%3D1")
  }

  @Test func rejectsOthers() {
    #expect(DeepLink(url: URL(string: "https://example.com")!) == nil)
    #expect(DeepLink(url: URL(string: "compass://nope")!) == nil)
    #expect(DeepLink(url: URL(string: "compass://open")!) == nil)
    #expect(DeepLink(url: URL(string: "compass://node")!) == nil)
  }
}

struct InboxTests {
  private func temp() -> URL {
    FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
  }

  @Test func drainsOldestFirstAndEmpties() throws {
    let inbox = Inbox(directory: temp())
    #expect(inbox.isEmpty)
    let later = InboxItem(action: .pin, url: "https://b.example", spaceID: "s", folderID: "f", date: Date(timeIntervalSince1970: 200))
    let first = InboxItem(action: .open, url: "https://a.example", title: "A", date: Date(timeIntervalSince1970: 100))
    try inbox.add(later)
    try inbox.add(first)
    #expect(!inbox.isEmpty)
    #expect(inbox.drain() == [first, later])
    #expect(inbox.isEmpty)
    #expect(inbox.drain().isEmpty)
  }

  @Test func dropsJunk() throws {
    let inbox = Inbox(directory: temp())
    try inbox.add(InboxItem(action: .open, url: "https://a.example"))
    try Data("{".utf8).write(to: inbox.directory.appendingPathComponent("bad.json"))
    #expect(inbox.drain().count == 1)
    #expect(inbox.isEmpty)
  }
}

struct ContainerTests {
  @Test func snapshotRoundTripAndUnchangedWrite() throws {
    let container = SharedContainer(root: FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString))
    #expect(container.readSnapshot() == nil)
    let space = SpaceInfo(
      id: "s", name: "Work", icon: .symbol("briefcase.fill"), style: SpaceStyle(stops: [0x112233], opacity: 0.3),
      favorites: [Entry(id: "f", title: "", url: "https://www.example.com/")],
      folders: [Folder(id: "a", name: "A", children: [Folder(id: "b", name: "B")])])
    let snapshot = Snapshot(written: Date(timeIntervalSince1970: 1), spaces: [space], recentSpaceIDs: ["s"])
    #expect(try container.write(snapshot))
    #expect(container.readSnapshot() == snapshot)
    var again = snapshot
    again.written = Date(timeIntervalSince1970: 2)
    #expect(try !container.write(again))
    #expect(space.favorites[0].label == "example.com")
    #expect(space.allFolders.map(\.folder.id) == ["a", "b"])
    #expect(space.allFolders.map(\.depth) == [0, 1])
  }

  @Test func focus() throws {
    let container = SharedContainer(root: FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString))
    #expect(container.readFocus() == nil)
    try container.write(FocusSetting(spaceID: "s", onlyThisSpace: true))
    #expect(container.readFocus() == FocusSetting(spaceID: "s", onlyThisSpace: true))
    try container.write(nil as FocusSetting?)
    #expect(container.readFocus() == nil)
  }

  @Test func recentSpaces() {
    let snapshot = Snapshot(
      spaces: ["a", "b", "c", "d"].map { SpaceInfo(id: $0, name: $0) }, recentSpaceIDs: ["c", "gone", "a"])
    #expect(snapshot.currentSpace?.id == "c")
    #expect(snapshot.recentSpaces(3).map(\.id) == ["c", "a", "b"])
  }
}
