// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassModel

/// Tidy Today in the store (docs/design/ai-sidebar.md, "Tidy Today"), as
/// the desktop's SpaceModelTest.TidyTodayAndUndo and UndoTidyAfterChanges.
@MainActor
struct TodayTidyTests {
  /// A space whose Today holds, in `today.children`'s order (the sidebar
  /// shows it the other way round): a, b, a folder holding f, a split of
  /// c and d, e, g; and a pinned tab.
  @MainActor final class Setup {
    let store = SpaceStore()
    let space: Space
    let a, b, c, d, e, f, g, pinned: SidebarNode
    let folder, split: SidebarNode
    var changes: [SpaceStoreChange] = []

    init() {
      space = store.addSpace(name: "Work")
      a = store.addTab(url: "https://a.example/", title: "A", to: space.today)!
      b = store.addTab(url: "https://b.example/", title: "B", to: space.today)!
      folder = store.addFolder(name: "Mine", to: space.today)!
      f = store.addTab(url: "https://f.example/", title: "F", to: folder)!
      c = store.addTab(url: "https://c.example/", title: "C", to: space.today)!
      d = store.addTab(url: "https://d.example/", title: "D", to: space.today)!
      split = store.split(c, with: d)!
      e = store.addTab(url: "https://e.example/", title: "E", to: space.today)!
      g = store.addTab(url: "https://g.example/", title: "G", to: space.today)!
      pinned = store.addTab(url: "https://p.example/", title: "P", to: space.pinned)!
      store.onLocalChange = { [unowned self] in self.changes.append($0) }
    }

    var today: [String] { space.today.children.map(\.id) }
  }

  @Test func tidiesIntoFoldersAndUndoesExactly() {
    let s = Setup()
    let before = s.today
    let tidy = s.store.tidyToday(
      in: s.space,
      groups: [
        .init(name: "Letters", entries: [s.a.id, s.e.id, s.split.id]),
        .init(name: "More", entries: [s.b.id, s.g.id]),
      ])
    // One change for sync.
    #expect(s.changes.count == 1)
    #expect(tidy.folders.count == 2 && tidy.before == before)
    let letters = s.store.node(id: tidy.folders[0])!
    let more = s.store.node(id: tidy.folders[1])!
    // Each folder where its topmost entry was (the end is Today's top), its
    // entries topmost first; the split whole; the user's folder untouched.
    #expect(s.today == [s.folder.id, letters.id, more.id])
    #expect(letters.children.map(\.id) == [s.e.id, s.split.id, s.a.id])
    #expect(more.children.map(\.id) == [s.g.id, s.b.id])
    #expect(s.split.children.map(\.id) == [s.c.id, s.d.id])
    #expect(s.folder.children.map(\.id) == [s.f.id] && s.folder.title == "Mine")
    #expect(s.pinned.parent === s.space.pinned)
    // Named by Claude, with Undo Name.
    #expect(letters.title == "Letters" && letters.nameSource == .claude)
    #expect(letters.nameBeforeClaude == SpaceStore.tidyFolderPlaceholder)

    s.changes = []
    #expect(s.store.undoTidy(tidy))
    #expect(s.changes.count == 1)
    #expect(s.today == before)
    #expect(s.store.node(id: letters.id) == nil && s.store.node(id: more.id) == nil)
    #expect(s.changes[0].deletedNodes == [letters.id, more.id])
    #expect(s.split.children.map(\.id) == [s.c.id, s.d.id] && s.folder.children.map(\.id) == [s.f.id])
    // Once.
    #expect(!s.store.undoTidy(tidy))
  }

  @Test func skipsWhatIsntALooseEntry() {
    let s = Setup()
    let tidy = s.store.tidyToday(
      in: s.space,
      groups: [
        // A folder, a tab in a folder, a pinned tab, a pane, an unknown id:
        // only b is left, which makes no folder.
        .init(name: "Nope", entries: [s.folder.id, s.f.id, s.pinned.id, s.c.id, "nope", s.b.id]),
        // Taken by an earlier group: a is, then only g is left.
        .init(name: "One", entries: [s.a.id, s.e.id]),
        .init(name: "Two", entries: [s.e.id, s.g.id]),
        // No name.
        .init(name: "", entries: [s.b.id, s.g.id]),
      ])
    #expect(tidy.folders.count == 1)
    let one = s.store.node(id: tidy.folders[0])!
    #expect(one.title == "One" && one.children.map(\.id) == [s.e.id, s.a.id])
    #expect(s.today == [s.b.id, s.folder.id, s.split.id, one.id, s.g.id])
  }

  @Test func undoAfterChanges() {
    let s = Setup()
    let tidy = s.store.tidyToday(in: s.space, groups: [.init(name: "Letters", entries: [s.a.id, s.e.id, s.b.id])])
    let letters = s.store.node(id: tidy.folders[0])!
    // Since: a new tab on top, one closed, another dragged into the tidied
    // folder.
    let new = s.store.addTab(url: "https://new.example/", title: "New", to: s.space.today)!
    s.store.remove(s.e)
    let dragged = s.store.addTab(url: "https://x.example/", title: "X", to: s.space.today)!
    s.store.move(dragged, to: letters, at: 0)
    #expect(s.store.undoTidy(tidy))
    #expect(s.store.node(id: letters.id) == nil)
    // From before in their order, then what came since on top.
    #expect(s.today == [s.a.id, s.b.id, s.folder.id, s.split.id, s.g.id, dragged.id, new.id])
  }

  @Test func undoWithItsFoldersGoneDoesNothing() {
    let s = Setup()
    let tidy = s.store.tidyToday(in: s.space, groups: [.init(name: "Pair", entries: [s.a.id, s.b.id])])
    s.store.remove(s.store.node(id: tidy.folders[0])!)
    s.changes = []
    #expect(!s.store.undoTidy(tidy))
    #expect(s.changes.isEmpty)
  }

  @Test func neverInIncognito() {
    let store = SpaceStore()
    let today = store.incognito.today
    let a = store.addTab(url: "https://a.example/", title: "A", to: today)!
    let b = store.addTab(url: "https://b.example/", title: "B", to: today)!
    #expect(!store.isLooseTodayEntry(a))
    let tidy = store.tidyToday(in: store.incognito, groups: [.init(name: "Pair", entries: [a.id, b.id])])
    #expect(tidy.folders.isEmpty && today.children.map(\.id) == [a.id, b.id])
  }
}
