// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Testing

@testable import CompassClaude

/// Claude's sidebar tools on iOS, as the desktop's
/// ClaudeToolsBrowserTest.OrganizesTheSidebar and SpacesAndUnloadedEntries.
@MainActor
struct SidebarToolsTests {
  func make() -> (SpaceStore, Space, SidebarTools) {
    let store = SpaceStore()
    let space = store.addSpace(name: "Work")
    return (store, space, SidebarTools(store: store, shownSpace: space))
  }

  func id(_ node: SidebarNode) -> JSONValue { .string(SidebarTools.nodeID(node)) }

  @Test func ids() {
    let (store, space, tools) = make()
    let tab = store.addTab(url: "https://a.example/", to: space.pinned)!
    let nid = SidebarTools.nodeID(tab)
    #expect(nid == "n" + tab.id.prefix(8))
    #expect(SidebarTools.spaceID(space) == "s" + space.id.prefix(8))
    // The desktop's id, the bare start, any case.
    #expect(tools.node(nid) === tab)
    #expect(tools.node(String(tab.id.prefix(8))) === tab)
    #expect(tools.node(nid.uppercased()) === tab)
    #expect(tools.node("nzzzzzzzz") == nil)
    #expect(tools.node("") == nil)
    #expect(tools.space(SidebarTools.spaceID(space)) === space)
    #expect(tools.space("work") === space)
    #expect(tools.space("Home") == nil)
    // Never incognito's or the archive's.
    let secret = store.addTab(url: "https://secret.example/", to: store.incognito.today)!
    #expect(tools.node(SidebarTools.nodeID(secret)) == nil)
    let old = store.addTab(url: "https://old.example/", to: space.today)!
    store.archive(old)
    #expect(tools.node(SidebarTools.nodeID(old)) == nil)
  }

  @Test func listsTheSidebar() {
    let (store, space, made) = make()
    var tools = made
    let other = store.addSpace(name: "Home")
    tools.otherWindowSpaces = [other.id]
    let fav = store.addTab(url: "https://fav.example/", title: "Fav", to: store.favorites)!
    let folder = store.addFolder(name: "Docs", to: space.pinned)!
    let inFolder = store.addTab(url: "https://doc.example/", title: "Doc", to: folder)!
    let older = store.addTab(url: "https://older.example/", title: "Older", to: space.today)!
    let newer = store.addTab(url: "https://newer.example/", title: "Newer", to: space.today)!
    let pane = store.addTab(url: "https://pane.example/", title: "Pane", to: space.today)!
    let split = store.split(older, with: pane)!
    store.rename(inFolder, customTitle: "My doc")
    let text = tools.listTabs { tab in
      tab === newer
        ? .init(title: "Newer live", url: "https://newer.example/live", loaded: true, front: true, target: true,
          opened: true, muted: true, playing: true)
        : .init()
    }
    let expected = """
      Favorites (every space):
        - \(SidebarTools.nodeID(fav)): Fav — https://fav.example/ [unloaded]

      Space "Work" (\(SidebarTools.spaceID(space))) [shown in the user's window]:
        Pinned:
          - folder "Docs" (folder_id \(SidebarTools.nodeID(folder))):
            - \(SidebarTools.nodeID(inFolder)): My doc — https://doc.example/ [unloaded]
        Today:
          - \(SidebarTools.nodeID(newer)): Newer live — https://newer.example/live [front] [you act on this] [muted] [playing] [you opened it]
          - split \(SidebarTools.nodeID(split)), side by side:
            - \(SidebarTools.nodeID(older)): Older — https://older.example/ [unloaded]
            - \(SidebarTools.nodeID(pane)): Pane — https://pane.example/ [unloaded]

      Space "Home" (\(SidebarTools.spaceID(other))) [shown in another window]:
        Pinned:
        Today:

      """
    #expect(text == expected)
    // Hidden entries (the phone's desktop-only pages) aren't listed.
    tools.isVisible = { $0 !== fav }
    #expect(!tools.listTabs { _ in .init() }.contains("Favorites"))
    #expect(tools.node(SidebarTools.nodeID(fav)) == nil)
  }

  @Test func organizesTheSidebar() {
    let (store, space, tools) = make()
    let a = store.addTab(url: "https://a.example/", to: space.today)!
    let b = store.addTab(url: "https://b.example/", to: space.today)!
    let c = store.addTab(url: "https://c.example/", to: space.today)!

    // Pin, then unpin; Today's index counts newest first, as listed.
    #expect(tools.move(["tab_id": id(a), "to": "pinned"]) == .text("Moved \(SidebarTools.nodeID(a))."))
    #expect(a.parent === space.pinned)
    #expect(!tools.move(["tab_id": id(a), "to": "today", "index": 0]).isError)
    #expect(space.today.children.reversed().map(\.id) == [a.id, c.id, b.id])
    #expect(!tools.move(["tab_id": id(a), "to": "today", "index": 2]).isError)
    #expect(space.today.children.reversed().map(\.id) == [c.id, b.id, a.id])

    // Reorder in Pinned.
    _ = tools.move(["tab_id": id(b), "to": "pinned"])
    _ = tools.move(["tab_id": id(c), "to": "pinned"])
    #expect(!tools.move(["tab_id": id(c), "to": "pinned", "index": 0]).isError)
    #expect(space.pinned.children.map(\.id) == [c.id, b.id])

    // A favorite, and back to the window's space.
    #expect(!tools.move(["tab_id": id(b), "to": "favorites"]).isError)
    #expect(b.parent === store.favorites)
    #expect(!tools.move(["tab_id": id(b), "to": "pinned"]).isError)
    #expect(b.parent === space.pinned)

    // A folder, a tab in it renamed, the folder renamed.
    let created = tools.createFolder(["name": "Reading"])
    let folder = space.pinned.children.first { $0.isFolder }!
    #expect(created == .text("Created folder \"Reading\": folder_id \(SidebarTools.nodeID(folder))."))
    #expect(!tools.move(["tab_id": id(c), "to": "folder", "folder_id": id(folder)]).isError)
    #expect(c.parent === folder)
    #expect(tools.rename(["tab_id": id(c), "title": " Later "]) == .text("Renamed \(SidebarTools.nodeID(c))."))
    #expect(c.customTitle == "Later")
    _ = tools.rename(["tab_id": id(c), "title": ""])
    #expect(c.customTitle == "")
    #expect(tools.rename(["tab_id": id(folder), "title": ""]) == .error("A folder needs a name."))
    _ = tools.rename(["tab_id": id(folder), "title": "Read"])
    #expect(folder.title == "Read")
    let inner = tools.createFolder(["name": "Inner", "in": id(folder)])
    #expect(!inner.isError && folder.children.contains { $0.isFolder })
    #expect(tools.createFolder(["name": "T", "in": "today"]).isError == false)
    #expect(tools.createFolder(["name": " "]) == .error("Give the folder a name."))
    #expect(tools.createFolder(["name": "X", "in": "nowhere"]) == .error("in is pinned, today, or a folder_id."))

    // Refused: unknown ids, folders as tabs, bad destinations.
    #expect(tools.move(["tab_id": "n00000000", "to": "pinned"]).isError)
    #expect(tools.move(["tab_id": id(folder), "to": "today"]).isError)
    #expect(tools.move(["tab_id": id(a), "to": "folder"]) == .error("Give the folder_id of a folder from list_tabs."))
    #expect(tools.move(["tab_id": id(a), "to": "elsewhere"]) == .error("to is pinned, today, favorites or folder."))
    #expect(tools.move(["tab_id": id(a)]) == .error("Give tab_id and to."))
  }

  @Test func splitsAndSpaces() {
    let (store, space, tools) = make()
    let a = store.addTab(url: "https://a.example/", to: space.today)!
    let b = store.addTab(url: "https://b.example/", to: space.today)!
    let fav = store.addTab(url: "https://fav.example/", to: store.favorites)!

    #expect(tools.split(["tab_id": id(b), "with_tab_id": id(a)]).text.hasSuffix("are side by side now; activate_tab shows them."))
    let split = a.parent!
    #expect(split.isSplit && split.children.map(\.id) == [a.id, b.id])
    #expect(tools.split(["tab_id": id(fav), "with_tab_id": id(a)]).isError)
    #expect(tools.rename(["tab_id": id(split), "title": "X"]) == .error("Rename one of the split's tabs."))
    // A split moves whole; a pane moves out of it.
    #expect(!tools.move(["tab_id": id(split), "to": "pinned"]).isError)
    #expect(split.parent === space.pinned)
    #expect(!tools.move(["tab_id": id(b), "to": "today"]).isError)
    #expect(b.parent === space.today)

    // A space, made in the shown space's profile, then a tab moved there.
    let (created, home) = tools.createSpace(["name": "Home"])
    #expect(created == .text("Created space \"Home\" (\(SidebarTools.spaceID(home!)))."))
    #expect(store.spaces.map(\.name) == ["Work", "Home"])
    #expect(!tools.move(["tab_id": id(b), "to": "pinned", "space": "home"]).isError)
    #expect(b.parent === home!.pinned)
    #expect(tools.move(["tab_id": id(b), "to": "today", "space": "Nowhere"]) == .error("No space \"Nowhere\"."))
    #expect(tools.createSpace(["name": ""]).0 == .error("Give the space a name."))
    // Tabs of two spaces can't be split.
    #expect(tools.split(["tab_id": id(b), "with_tab_id": id(a)]).isError)
    // Folders in another space.
    #expect(!tools.createFolder(["name": "F", "space": "Home"]).isError)
    #expect(home!.pinned.children.contains { $0.isFolder })
  }

  @Test func historyAndDownloadLines() {
    let date = Date(timeIntervalSince1970: 1_800_000_000)
    let when = date.formatted(date: .numeric, time: .shortened)
    #expect(SidebarTools.historyLine(title: "A", url: "https://a/", date: date) == "- A — https://a/ (\(when))\n")
    #expect(
      SidebarTools.downloadLine(
        name: "f.pdf", state: "done", bytes: 10, total: 10, url: nil, savedAt: "Downloads", date: date)
        == "- f.pdf (done, 10 of 10 bytes), saved at Downloads, \(when)\n")
    #expect(
      SidebarTools.downloadLine(
        name: "g.zip", state: "in progress", bytes: 5, total: nil, url: "https://g/", savedAt: "D", date: date)
        == "- g.zip (in progress, 5 bytes) from https://g/, saved at D, \(when)\n")
  }
}
