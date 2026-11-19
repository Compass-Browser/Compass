// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassEasels
import Foundation
import SyncProto

/// Easels in COMPASS_SPACES (docs/design/easels.md, "Sync"): one entity per
/// board, per object and per image, in the account of the profile they
/// belong to. The phone keeps one set of boards, so only the home account's
/// bridge carries them, as Boosts (+Boosts); other profiles' stay on their
/// desktops.
///
/// The store keeps the entities as they came (unknown fields too), so they
/// go as they are, not over the mirror. An image never changes (it's named
/// by its hash): one the account has is never sent again, and its bytes go
/// only with its first commit; the mirror keeps images without them.
extension CompassSpacesBridge {
  static func isEasel(_ s: SyncPb_CompassSpacesSpecifics) -> Bool {
    switch s.entity {
    case .easel, .easelObject, .easelAsset: true
    default: false
    }
  }

  /// What the mirror keeps of an entity: an Easel image without its bytes.
  static func mirrored(_ s: SyncPb_CompassSpacesSpecifics) -> SyncPb_CompassSpacesSpecifics {
    guard case .easelAsset(var asset) = s.entity, asset.hasData else { return s }
    asset.clearData()
    var out = s
    out.easelAsset = asset
    return out
  }

  /// The entities to send for `easels`: boards and objects as they are,
  /// images the account doesn't have with their bytes (left out while
  /// those can't be read).
  @MainActor
  func easelSpecifics(_ easels: EaselEntities) -> [SyncPb_CompassSpacesSpecifics] {
    var out: [SyncPb_CompassSpacesSpecifics] = []
    for easel in easels.easels where Self.isValidUUID(easel.uuid) {
      var s = SyncPb_CompassSpacesSpecifics()
      s.easel = easel
      out.append(s)
    }
    for object in easels.objects where Self.isValidUUID(object.uuid) {
      var s = SyncPb_CompassSpacesSpecifics()
      s.easelObject = object
      out.append(s)
    }
    for asset in easels.assets where Self.isValidUUID(asset.uuid) {
      if case .easelAsset? = mirror[asset.uuid]?.entity { continue }
      guard let data = easelAssetData?(asset.sha256) else { continue }
      var full = asset
      full.data = data
      var s = SyncPb_CompassSpacesSpecifics()
      s.easelAsset = full
      out.append(s)
    }
    return out
  }

  /// The Easels changed here (or a remote change was applied): commits
  /// what differs from the account's, deletes what's gone.
  @MainActor
  public func easelsChanged(_ easels: EaselEntities) {
    guard isHome, tracking else { return }
    let ids = Set(easels.easels.map(\.uuid) + easels.objects.map(\.uuid) + easels.assets.map(\.uuid))
    for (id, s) in mirror where Self.isEasel(s) && !ids.contains(id) {
      mirror[id] = nil
      lastCommit[id] = nil
      queueDelete(id)
    }
    for s in easelSpecifics(easels) {
      guard let id = Self.uuid(of: s) else { continue }
      if case .easelAsset = s.entity {
        queuePut(id, s, mirrored: Self.mirrored(s))
      } else {
        commit(id, s)
      }
    }
    drain()
  }
}
