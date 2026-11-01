// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import SyncProto

/// A profile of the chain (components/sync/profile_list.h, ChainProfile):
/// the home profile's COMPASS_SPACES lists them as CompassProfile entities,
/// and pairing's message 4 carries the list.
public struct ChainProfile: Sendable, Equatable, Codable, Identifiable {
  /// Lowercase UUID. The account is HKDF(seed, "compass-sync-auth:" + uuid),
  /// except for the home profile's, which is the chain's own.
  public var uuid: String
  public var name: String
  /// Chromium's default avatar icon index + 1; nil if not set.
  public var avatar: Int32?
  public var home: Bool
  public var id: String { uuid }

  public init(uuid: String, name: String, avatar: Int32? = nil, home: Bool = false) {
    self.uuid = uuid
    self.name = name
    self.avatar = avatar
    self.home = home
  }

  init(_ p: SyncPb_CompassProfile) {
    self.init(uuid: p.uuid, name: p.name, avatar: p.hasAvatar ? p.avatar : nil, home: p.home)
  }

  /// Pairing's `"profiles": [{"uuid", "name", "avatar", "home"}]`. Like
  /// ChainProfilesFromValue(): entries without a valid lowercase UUID, and
  /// repeats of one, are skipped.
  public static func list(json: Any?) -> [ChainProfile] {
    guard let entries = json as? [Any] else { return [] }
    var seen: Set<String> = []
    var out: [ChainProfile] = []
    for case let entry as [String: Any] in entries {
      guard let uuid = entry["uuid"] as? String, CompassSpacesBridge.isValidUUID(uuid), seen.insert(uuid).inserted
      else { continue }
      out.append(
        ChainProfile(
          uuid: uuid, name: entry["name"] as? String ?? "", avatar: (entry["avatar"] as? NSNumber)?.int32Value,
          home: entry["home"] as? Bool ?? false))
    }
    return out
  }

  /// The chain's home UUID: the one entity marked home, or the smallest of
  /// several (two devices that joined at once; the desktop keeps the
  /// smallest and deletes the other).
  public static func homeUUID(_ profiles: [ChainProfile]) -> String? {
    profiles.filter(\.home).map(\.uuid).min()
  }

  /// The home first, then the others by name.
  public static func sorted(_ profiles: [ChainProfile]) -> [ChainProfile] {
    let home = homeUUID(profiles)
    return profiles.filter { !$0.home || $0.uuid == home }.sorted {
      ($0.uuid == home ? 0 : 1, $0.name.lowercased(), $0.uuid) < ($1.uuid == home ? 0 : 1, $1.name.lowercased(), $1.uuid)
    }
  }
}

/// Every profile's spaces in one SpaceStore, as the desktop's SpaceDirectory
/// spans profiles in its switcher (docs/design/ios.md, "M8"). It holds one
/// CompassSpacesBridge per profile account and decides which one owns each
/// space and node:
///
/// - a space belongs to the bridge of its profile UUID; the home profile's
///   (empty or the home UUID) and any profile without a bridge here, to the
///   home bridge;
/// - a node, to its space's bridge; favorites, to their profile's.
///
/// Local changes go to every bridge (each commits what it owns, and deletes
/// what it had); after any change, local or remote, `reconcile()` has each
/// bridge give up what another one owns now (a space or entry moved to
/// another profile), so its account deletes it.
///
/// Positions compare across accounts: the store keeps every profile's spaces
/// in one `UniquePosition` order, so a space placed between two others gets
/// a position between theirs, whichever accounts they're in (the desktop's
/// GetDirectoryOrder() + SortDirectoryByPositions()).
@MainActor
public final class SpaceDirectory {
  public let store: SpaceStore
  private var refs: [WeakBridge] = []
  /// The chain's profiles, from the home account (home first).
  public private(set) var profiles: [ChainProfile] = []
  /// The chain's profile list changed (from the home account).
  public var onProfilesChanged: (([ChainProfile]) -> Void)?

  struct WeakBridge {
    weak var bridge: CompassSpacesBridge?
  }

  public init(store: SpaceStore) {
    self.store = store
    store.suffixForID = CompassSpacesBridge.suffix(for:)
    store.onLocalChange = { [weak self] change in self?.localChanged(change) }
  }

  public var bridges: [CompassSpacesBridge] { refs.compactMap(\.bridge) }
  public var home: CompassSpacesBridge? { bridges.first { $0.isHome } }

  func add(_ bridge: CompassSpacesBridge) {
    refs.removeAll { $0.bridge == nil }
    refs.append(WeakBridge(bridge: bridge))
    if bridge.isHome { homeProfilesChanged() }
  }

  /// Stops routing to a bridge (its account left).
  public func remove(_ bridge: CompassSpacesBridge) {
    refs.removeAll { $0.bridge == nil || $0.bridge === bridge }
  }

  // MARK: Ownership

  /// The bridge that syncs a profile's spaces here.
  public func owner(profile: String) -> CompassSpacesBridge? {
    if !store.isHome(profile), let bridge = bridges.first(where: { !$0.isHome && $0.accountProfile == profile }) {
      return bridge
    }
    return home
  }

  public func owner(of space: Space) -> CompassSpacesBridge? { owner(profile: space.profileUUID) }

  public func owner(of node: SidebarNode) -> CompassSpacesBridge? { owner(profile: store.profile(of: node)) }

  /// The owner of every space and node in the store, by ID.
  func owners() -> [String: ObjectIdentifier] {
    var out: [String: ObjectIdentifier] = [:]
    for space in store.spaces {
      guard let owner = owner(of: space) else { continue }
      let id = ObjectIdentifier(owner)
      out[space.id] = id
      for node in space.allNodes { out[node.id] = id }
    }
    for root in store.favoritesRoots {
      guard let owner = owner(of: root) else { continue }
      for node in root.children.flatMap(\.subtree) { out[node.id] = ObjectIdentifier(owner) }
    }
    return out
  }

  // MARK: Changes

  func localChanged(_ change: SpaceStoreChange) {
    for bridge in bridges { bridge.localChanged(change) }
    reconcile()
  }

  /// Each bridge drops (and deletes from its account) what another owns now.
  public func reconcile() {
    let owners = owners()
    for bridge in bridges { bridge.release(owners: owners) }
  }

  /// The home account's CompassProfile entities changed (or were loaded).
  func homeProfilesChanged() {
    guard let home else { return }
    let list = ChainProfile.sorted(home.mirrorProfiles())
    guard list != profiles else { return }
    profiles = list
    if let uuid = ChainProfile.homeUUID(list), uuid != store.homeProfileUUID {
      store.homeProfileUUID = uuid
      home.homeUUIDChanged()
    }
    onProfilesChanged?(list)
  }

  /// The profile list from pairing (message 4), before the home account's
  /// first sync brings it: the home UUID is known at once.
  public func adoptPairingProfiles(_ list: [ChainProfile]) {
    if store.homeProfileUUID.isEmpty, let uuid = ChainProfile.homeUUID(list) {
      store.homeProfileUUID = uuid
      home?.homeUUIDChanged()
    }
  }
}
