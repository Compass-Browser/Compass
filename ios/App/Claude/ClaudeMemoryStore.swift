// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import Foundation
import Observation

/// What Claude remembers about the user (docs/design/claude.md, "Memory"),
/// in Application Support/Compass/Claude Memory.json, synced with the home
/// account's spaces (CompassSpacesBridge+Memory). The rules are
/// CompassClaude's `MemoryList`, the desktop's.
@MainActor
@Observable
final class ClaudeMemoryStore {
  static let shared = ClaudeMemoryStore()

  private(set) var list: MemoryList
  var entries: [MemoryEntry] { list.entries }

  /// After every change, local or remote: sync commits what differs.
  @ObservationIgnored var onChange: (([MemoryEntry]) -> Void)?

  @ObservationIgnored private let url: URL

  private init() {
    let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Compass", isDirectory: true)
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    url = directory.appendingPathComponent("Claude Memory.json")
    list = (try? Data(contentsOf: url)).flatMap { try? JSONDecoder().decode(MemoryList.self, from: $0) }
      .map { MemoryList(entries: $0.entries) } ?? MemoryList()
  }

  /// The <memory> block a new chat starts with.
  var promptBlock: String { list.promptBlock }

  func find(_ id: String) -> MemoryEntry? { list.find(id) }

  func add(_ text: String) -> Result<MemoryEntry, MemoryList.Failure> {
    let count = list.entries.count
    let result = list.add(text)
    if list.entries.count != count { changed() }
    return result
  }

  func update(_ id: String, text: String) -> Result<MemoryEntry, MemoryList.Failure> {
    let before = list
    let result = list.update(id, text: text)
    if list != before { changed() }
    return result
  }

  @discardableResult
  func remove(_ id: String) -> MemoryEntry? {
    let removed = list.remove(id)
    if removed != nil { changed() }
    return removed
  }

  func removeAll() {
    guard !list.entries.isEmpty else { return }
    list.removeAll()
    changed()
  }

  @discardableResult
  func restore(_ entry: MemoryEntry) -> Bool {
    let restored = list.restore(entry)
    if restored { changed() }
    return restored
  }

  func applyRemote(_ upserts: [MemoryEntry], deleted: Set<String>) {
    if list.applyRemote(upserts, deleted: deleted) { changed() }
  }

  private func changed() {
    if let data = try? JSONEncoder().encode(list) {
      try? data.write(to: url, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    }
    onChange?(list.entries)
  }
}
