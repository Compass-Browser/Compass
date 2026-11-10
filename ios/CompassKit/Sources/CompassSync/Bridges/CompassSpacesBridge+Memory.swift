// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import Foundation
import SyncProto

/// Claude's memory in COMPASS_SPACES (docs/design/claude.md, "Memory"): one
/// `CompassMemory` per fact, in the account of the profile it belongs to.
/// The phone has one Claude, so only the home account's bridge carries
/// memories; other profiles' stay on their desktops.
extension CompassSpacesBridge {
  static func memoryEntry(_ m: SyncPb_CompassMemory) -> MemoryEntry {
    MemoryEntry(
      id: m.uuid, text: m.text, created: date(windowsMicros: m.createTime), updated: date(windowsMicros: m.updateTime))
  }

  /// What we'd commit for a memory: the mirror with our fields over it.
  @MainActor
  func desired(_ memory: MemoryEntry) -> SyncPb_CompassSpacesSpecifics {
    var specifics = mirror[memory.id] ?? SyncPb_CompassSpacesSpecifics()
    var m = specifics.memory
    m.uuid = memory.id
    m.text = memory.text
    m.createTime = Self.windowsMicros(memory.created)
    m.updateTime = Self.windowsMicros(memory.updated)
    specifics.memory = m
    return specifics
  }

  /// The memories changed here (or a remote change was applied): commits
  /// what differs from the account's, deletes what's gone.
  @MainActor
  public func memoriesChanged(_ memories: [MemoryEntry]) {
    guard isHome, tracking else { return }
    let ids = Set(memories.map(\.id))
    for (id, s) in mirror where !ids.contains(id) {
      guard case .memory = s.entity else { continue }
      mirror[id] = nil
      lastCommit[id] = nil
      queueDelete(id)
    }
    for memory in memories where Self.isValidUUID(memory.id) { commit(memory.id, desired(memory)) }
    drain()
  }
}
