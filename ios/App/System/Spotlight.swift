// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import CoreSpotlight
import UniformTypeIdentifiers

/// Favorites and Pinned entries in Spotlight (Home Screen search). An item's
/// ID is its sidebar node's, so a tap shows that entry; its domain is its
/// space's. The index follows the snapshot: what left the sidebar leaves it.
@MainActor
final class Spotlight {
  static let enabledKey = "CompassSpotlight"
  /// Settings → "Show in Spotlight"; on unless turned off.
  static var isEnabled: Bool { UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true }

  private static let indexedKey = "CompassSpotlightIndexed"
  private let index = CSSearchableIndex(name: "Compass")

  /// The IDs indexed last time.
  private var indexed: Set<String> {
    get { Set(UserDefaults.standard.stringArray(forKey: Self.indexedKey) ?? []) }
    set { UserDefaults.standard.set(Array(newValue), forKey: Self.indexedKey) }
  }
  func update(_ snapshot: Snapshot, icons: SharedContainer, enabled: Bool) {
    guard enabled else {
      if !indexed.isEmpty {
        index.deleteAllSearchableItems { _ in }
        indexed = []
        lastSignature = [:]
      }
      return
    }
    var items: [String: CSSearchableItem] = [:]
    var signature: [String: String] = [:]
    for space in snapshot.spaces {
      for (entry, kind) in space.favorites.map({ ($0, "Favorite") }) + space.pinned.map({ ($0, "Pinned") }) {
        guard items[entry.id] == nil else { continue }
        let attributes = CSSearchableItemAttributeSet(contentType: .url)
        attributes.title = entry.label
        attributes.contentDescription = "\(kind) in \(space.name) · \(entry.host)"
        attributes.url = URL(string: entry.url)
        attributes.keywords = [space.name, entry.host]
        if let png = try? Data(contentsOf: icons.icon(host: entry.iconHost)) {
          attributes.thumbnailData = png
        }
        let item = CSSearchableItem(uniqueIdentifier: entry.id, domainIdentifier: space.id, attributeSet: attributes)
        items[entry.id] = item
        signature[entry.id] = "\(entry.label)|\(entry.url)|\(space.name)|\(kind)"
      }
    }
    let gone = indexed.subtracting(items.keys)
    if !gone.isEmpty { index.deleteSearchableItems(withIdentifiers: Array(gone)) { _ in } }
    // Only what changed since the last update (all of it after a launch).
    let changed = items.filter { id, _ in lastSignature[id] != signature[id] }.map(\.value)
    if !changed.isEmpty { index.indexSearchableItems(changed) { _ in } }
    indexed = Set(items.keys)
    lastSignature = signature
  }

  private var lastSignature: [String: String] = [:]
}
