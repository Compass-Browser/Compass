// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import Foundation
import Testing

@testable import CompassClaude

struct ClaudeMemoryTests {
  @Test func addUpdateRemoveRestore() throws {
    var list = MemoryList()
    let italian = try list.add("  Prefers   answers\nin Italian ").get()
    #expect(italian.text == "Prefers answers in Italian")
    // The same fact again is the same memory.
    let again = try list.add("prefers answers in italian").get()
    #expect(again.id == italian.id)
    _ = try list.add("Lives in Milan").get()
    #expect(list.entries.count == 2)

    // By the short id the prompt shows.
    let updated = try list.update(String(italian.id.prefix(8)), text: "Prefers Italian").get()
    #expect(updated.id == italian.id)
    let missing = list.update("0000", text: "x")
    #expect(missing == .failure(.notFound("0000")))

    let removedEntry = list.remove(italian.id)
    let removed = try #require(removedEntry)
    #expect(list.entries.count == 1)
    let restored = list.restore(removed)
    #expect(restored)
    #expect(list.entries.first?.id == italian.id)
    list.removeAll()
    #expect(list.entries.isEmpty)
  }

  @Test func limits() throws {
    var list = MemoryList()
    let empty = list.add("   ")
    #expect(empty == .failure(.empty))
    let long = list.add(String(repeating: "a", count: MemoryList.maxChars + 1))
    #expect(long == .failure(.tooLong))
    _ = try list.add(String(repeating: "a", count: MemoryList.maxChars)).get()
    for i in 1..<MemoryList.maxEntries { _ = try list.add("Fact \(i)").get() }
    let full = list.add("One more")
    #expect(full == .failure(.full))
  }

  @Test func remoteChangesLaterEditWins() throws {
    var list = MemoryList()
    let now = Date()
    let a = MemoryEntry(id: UUID().uuidString.lowercased(), text: "A", created: now, updated: now)
    let b = MemoryEntry(id: UUID().uuidString.lowercased(), text: "B", created: now, updated: now)
    let added = list.applyRemote([a, b], deleted: [])
    #expect(added)
    _ = try list.update(a.id, text: "A local", now: now.addingTimeInterval(5)).get()

    var stale = a
    stale.text = "A stale"
    stale.updated = now.addingTimeInterval(1)
    let staleChanged = list.applyRemote([stale], deleted: [])
    #expect(!staleChanged)
    #expect(list.find(a.id)?.text == "A local")

    var fresh = a
    fresh.text = "A fresh"
    fresh.updated = now.addingTimeInterval(60)
    let freshChanged = list.applyRemote([fresh], deleted: [b.id])
    #expect(freshChanged)
    #expect(list.find(a.id)?.text == "A fresh")
    #expect(list.find(b.id) == nil)
  }

  @Test func promptBlockAndRequest() {
    #expect(MemoryList().promptBlock.isEmpty)
    let entry = MemoryEntry(
      id: "0123abcd-0000-4000-8000-000000000000", text: "Prefers Italian", created: Date(), updated: Date())
    let block = MemoryList(entries: [entry]).promptBlock
    #expect(block.hasPrefix("<memory>\n"))
    #expect(block.hasSuffix("</memory>"))
    #expect(block.contains("- [0123abcd] Prefers Italian\n"))

    // After the prompt, the one cached; the per-request context after it.
    let options = ChatRequest.Options(model: "m", systemPrompt: "p", claudeAPI: true, memory: block)
    let system = ChatRequest(messages: [["role": "user", "content": "x"]], options: options).body["system"]?.array ?? []
    #expect(system.count == 3)
    #expect(system[0]["cache_control"] == nil)
    #expect(system[1]["text"]?.string == block)
    #expect(system[1]["cache_control"] != nil)
  }
}
