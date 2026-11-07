// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

@testable import CompassModel
import Foundation
import Testing

@MainActor
struct ClosedEntriesTests {
  func make() -> (SpaceStore, Space, () -> [SpaceStoreChange]) {
    let store = SpaceStore()
    var log: [SpaceStoreChange] = []
    store.onLocalChange = { log.append($0) }
    let space = store.addSpace(name: "Work")
    log.removeAll()
    return (store, space, { log })
  }

  /// Records `nodes` as one step and closes each as given.
  func close(_ nodes: [(SidebarNode, ClosedEntries.Close)], in store: SpaceStore) -> ClosedEntries.Step {
    var step = ClosedEntries.Step()
    for (node, close) in nodes {
      step.entries.append(ClosedEntries.Entry(node, close: close, in: store))
      switch close {
      case .remove: store.remove(node)
      case .archive: store.archive(node)
      case .unload: store.goBackToSavedURL(node)
      }
    }
    return step
  }

  @Test func removedTabComesBackInPlaceUnderItsID() {
    let (store, space, changes) = make()
    let a = store.addTab(url: "https://a", to: space.pinned)!
    let b = store.addTab(url: "https://b", to: space.pinned)!
    let c = store.addTab(url: "https://c", to: space.pinned)!
    store.rename(b, customTitle: "Bee")
    let step = close([(b, .remove)], in: store)
    #expect(space.pinned.children.map(\.id) == [a.id, c.id])
    ClosedEntries.restore(step, in: store)
    #expect(space.pinned.children.map(\.id) == [a.id, b.id, c.id])
    #expect(store.node(id: b.id)?.customTitle == "Bee")
    #expect(changes().last?.putNodes.contains(b.id) == true)
  }

  @Test func archivedEntryMovesBackToToday() {
    let (store, space, _) = make()
    let a = store.addTab(url: "https://a", to: space.today)!
    let b = store.addTab(url: "https://b", to: space.today)!
    let step = close([(a, .archive)], in: store)
    #expect(a.section == .archive)
    ClosedEntries.restore(step, in: store)
    #expect(space.today.children.map(\.id) == [a.id, b.id])
    #expect(a.archivedAt == nil)
    #expect(space.archive.children.isEmpty)
  }

  @Test func clearTodayComesBackInOrder() {
    let (store, space, _) = make()
    let tabs = (0..<4).map { store.addTab(url: "https://\($0)", to: space.today)! }
    let step = close(tabs.map { ($0, .archive) }, in: store)
    #expect(space.today.children.isEmpty)
    ClosedEntries.restore(step, in: store)
    #expect(space.today.children.map(\.id) == tabs.map(\.id))
  }

  @Test func folderComesBackWithItsTree() {
    let (store, space, _) = make()
    let folder = store.addFolder(name: "F", to: space.pinned)!
    let inner = store.addTab(url: "https://in", to: folder)!
    store.addTab(url: "https://after", to: space.pinned)
    let step = close([(folder, .remove)], in: store)
    #expect(store.node(id: inner.id) == nil)
    ClosedEntries.restore(step, in: store)
    #expect(space.pinned.children.first === folder)
    #expect(store.node(id: inner.id) === inner)
  }

  @Test func paneComesBackSplitWithTheOther() {
    let (store, space, _) = make()
    let a = store.addTab(url: "https://a", to: space.today)!
    let b = store.addTab(url: "https://b", to: space.today)!
    let split = store.split(a, with: b)!
    let step = close([(a, .remove)], in: store)
    #expect(b.parent === space.today)
    ClosedEntries.restore(step, in: store)
    let again = try! #require(b.parent)
    #expect(again.isSplit && again !== split)
    #expect(again.children.map(\.id) == [a.id, b.id])
  }

  @Test func parentGoneSendsItToTopOfToday() {
    let (store, space, _) = make()
    store.addTab(url: "https://x", to: space.today)
    let folder = store.addFolder(name: "F", to: space.today)!
    let tab = store.addTab(url: "https://t", to: folder)!
    let step = close([(tab, .remove)], in: store)
    // The emptied Today folder closed with it.
    #expect(store.node(id: folder.id) == nil)
    ClosedEntries.restore(step, in: store)
    #expect(space.today.children.first === tab)
  }

  @Test func pinnedTabGoesBackToItsPage() {
    let (store, space, _) = make()
    let tab = store.addTab(url: "https://home", title: "Home", to: space.pinned)!
    store.update(tab, url: "https://away", title: "Away")
    let step = close([(tab, .unload)], in: store)
    #expect(tab.url == "https://home")
    ClosedEntries.restore(step, in: store, isOpen: { _ in true })
    #expect(tab.url == "https://home")
    ClosedEntries.restore(step, in: store)
    #expect(tab.url == "https://away" && tab.title == "Away")
  }

  @Test func reopensTheTabIfItsBack() {
    let (store, space, _) = make()
    let tab = store.addTab(url: "https://a", to: space.today)!
    var step = close([(tab, .archive)], in: store)
    step.reopenID = tab.id
    #expect(ClosedEntries.restore(step, in: store) === tab)
  }

  @Test func incognitoStaysIncognito() {
    let (store, space, _) = make()
    let tab = store.addTab(url: "https://secret", to: store.incognito.today)!
    let step = close([(tab, .archive)], in: store)
    #expect(store.node(id: tab.id) == nil)
    ClosedEntries.restore(step, in: store)
    #expect(tab.parent === store.incognito.today)
    #expect(space.today.children.isEmpty)
    // Not reinserted elsewhere either.
    store.remove(tab)
    #expect(!store.reinsert(tab, into: space.today, at: 0))
  }

  @Test func stepsAndRedo() {
    let closed = ClosedEntries()
    let (store, space, _) = make()
    let a = store.addTab(url: "https://a", to: space.today)!
    let b = store.addTab(url: "https://b", to: space.today)!
    closed.push(close([(a, .remove)], in: store))
    closed.push(close([(b, .remove)], in: store))
    let last = try! #require(closed.takeUndo())
    #expect(last.entries.first?.node === b)
    closed.pushRedo(last)
    #expect(closed.canRedo)
    // A redo's close keeps the other redos; any other drops them.
    closed.push(close([], in: store), keepRedo: false)
    #expect(closed.canRedo)
    closed.push(last, keepRedo: true)
    #expect(closed.canRedo)
    closed.push(last)
    #expect(!closed.canRedo)
  }

  @Test func keepsTheLastSteps() {
    let closed = ClosedEntries()
    let (store, space, _) = make()
    for i in 0..<(ClosedEntries.maxSteps + 5) {
      let tab = store.addTab(url: "https://\(i)", to: space.today)!
      closed.push(close([(tab, .remove)], in: store))
    }
    #expect(closed.undoSteps.count == ClosedEntries.maxSteps)
    #expect(closed.undoSteps.first?.entries.first?.node.url == "https://5")
  }

  @Test func dropsIncognitoSteps() {
    let closed = ClosedEntries()
    let (store, space, _) = make()
    let normal = store.addTab(url: "https://a", to: space.today)!
    let secret = store.addTab(url: "https://s", to: store.incognito.today)!
    closed.push(close([(normal, .remove)], in: store))
    closed.push(close([(secret, .remove)], in: store))
    closed.dropIncognito()
    #expect(closed.undoSteps.count == 1)
    #expect(closed.undoSteps.first?.entries.first?.node === normal)
  }

  @Test func titles() {
    let (store, space, _) = make()
    let a = store.addTab(url: "https://a", title: "A", to: space.today)!
    let b = store.addTab(url: "https://b", to: space.today)!
    #expect(close([(a, .remove)], in: store).title == "“A”")
    let c = store.addTab(url: "https://c", to: space.today)!
    #expect(close([(b, .remove), (c, .remove)], in: store).title == "2 tabs")
  }
}
