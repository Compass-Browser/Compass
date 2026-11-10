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
    #expect(try list.add("prefers answers in italian").get().id == italian.id)
    _ = try list.add("Lives in Milan").get()
    #expect(list.entries.count == 2)

    // By the short id the prompt shows.
    #expect(try list.update(String(italian.id.prefix(8)), text: "Prefers Italian").get().id == italian.id)
    #expect(list.update("0000", text: "x") == .failure(.notFound("0000")))

    let removed = try #require(list.remove(italian.id))
    #expect(list.entries.count == 1)
    #expect(list.restore(removed))
    #expect(list.entries.first?.id == italian.id)
    list.removeAll()
    #expect(list.entries.isEmpty)
  }

  @Test func limits() throws {
    var list = MemoryList()
    #expect(list.add("   ") == .failure(.empty))
    #expect(list.add(String(repeating: "a", count: MemoryList.maxChars + 1)) == .failure(.tooLong))
    _ = try list.add(String(repeating: "a", count: MemoryList.maxChars)).get()
    for i in 1..<MemoryList.maxEntries { _ = try list.add("Fact \(i)").get() }
    #expect(list.add("One more") == .failure(.full))
  }

  @Test func remoteChangesLaterEditWins() throws {
    var list = MemoryList()
    let now = Date()
    let a = MemoryEntry(id: UUID().uuidString.lowercased(), text: "A", created: now, updated: now)
    let b = MemoryEntry(id: UUID().uuidString.lowercased(), text: "B", created: now, updated: now)
    #expect(list.applyRemote([a, b], deleted: []))
    _ = try list.update(a.id, text: "A local", now: now.addingTimeInterval(5)).get()

    var stale = a
    stale.text = "A stale"
    stale.updated = now.addingTimeInterval(1)
    #expect(!list.applyRemote([stale], deleted: []))
    #expect(list.find(a.id)?.text == "A local")

    var fresh = a
    fresh.text = "A fresh"
    fresh.updated = now.addingTimeInterval(60)
    #expect(list.applyRemote([fresh], deleted: [b.id]))
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
