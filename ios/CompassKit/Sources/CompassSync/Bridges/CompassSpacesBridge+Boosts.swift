// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import Foundation
import SyncProto

/// Boosts in COMPASS_SPACES (docs/design/boosts.md, "Sync"): one
/// `CompassBoost` per Boost, in the account of the profile it belongs to.
/// The phone keeps one list of Boosts, so only the home account's bridge
/// carries them, as Claude's memory (+Memory); other profiles' stay on
/// their desktops.
extension CompassSpacesBridge {
  static func boost(_ b: SyncPb_CompassBoost) -> Boost {
    Boost(
      id: b.id, name: b.name, match: b.match, css: b.css, js: b.js,
      jsWorld: Boost.World(rawValue: b.jsWorld) ?? .isolated, enabled: b.hasEnabled ? b.enabled : true,
      author: Boost.Author(rawValue: b.author) ?? .user, createTime: b.createTime,
      updateTime: b.hasUpdateTime ? b.updateTime : b.createTime)
  }

  /// What we'd commit for a Boost: the mirror with our fields over it.
  @MainActor
  func desired(_ boost: Boost) -> SyncPb_CompassSpacesSpecifics {
    var specifics = mirror[boost.id] ?? SyncPb_CompassSpacesSpecifics()
    var b = specifics.boost
    b.id = boost.id
    b.name = boost.name
    b.match = boost.match
    b.css = boost.css
    b.js = boost.js
    b.jsWorld = boost.jsWorld.rawValue
    b.enabled = boost.enabled
    b.author = boost.author.rawValue
    b.createTime = boost.createTime
    b.updateTime = boost.updateTime
    specifics.boost = b
    return specifics
  }

  /// The Boosts changed here (or a remote change was applied): commits
  /// what differs from the account's, deletes what's gone.
  @MainActor
  public func boostsChanged(_ boosts: [Boost]) {
    guard isHome, tracking else { return }
    let ids = Set(boosts.map(\.id))
    for (id, s) in mirror where !ids.contains(id) {
      guard case .boost = s.entity else { continue }
      mirror[id] = nil
      lastCommit[id] = nil
      queueDelete(id)
    }
    for boost in boosts where Self.isValidUUID(boost.id) { commit(boost.id, desired(boost)) }
    drain()
  }
}
