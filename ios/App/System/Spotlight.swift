// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import CoreSpotlight
import UniformTypeIdentifiers

/// Spaces, favorites and Pinned entries in Spotlight (Home Screen search). An item's
/// ID is its sidebar node's, so a tap shows that entry; its domain is its
/// space's. The index follows the snapshot: what left the sidebar leaves it.
/// One more item stands for the app itself, so a search for "browser" finds
/// Compass (the Home Screen name alone doesn't have the word).
@MainActor
final class Spotlight {
  /// The app's own item: a tap just opens the app.
  static let appItemID = "compass.app"
  /// A space's item ID is this plus the space's ID.
  static let spacePrefix = "space:"
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
    if !appIndexed {
      index.indexSearchableItems([Self.appItem]) { _ in }
      appIndexed = true
    }
    guard enabled else {
      if !indexed.isEmpty {
        index.deleteSearchableItems(withIdentifiers: Array(indexed)) { _ in }
        indexed = []
        lastSignature = [:]
      }
      return
    }
    var items: [String: CSSearchableItem] = [:]
    var signature: [String: String] = [:]
    for space in snapshot.spaces {
      // The space itself: a tap shows its sidebar.
      let spaceID = Self.spacePrefix + space.id
      let spaceAttributes = CSSearchableItemAttributeSet(contentType: .content)
      spaceAttributes.title = space.name
      spaceAttributes.contentDescription = "Space in Compass"
      spaceAttributes.keywords = ["space", "Compass"]
      items[spaceID] = CSSearchableItem(uniqueIdentifier: spaceID, domainIdentifier: space.id, attributeSet: spaceAttributes)
      signature[spaceID] = "space|\(space.name)"
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
  private var appIndexed = false

  private static var appItem: CSSearchableItem {
    let attributes = CSSearchableItemAttributeSet(contentType: .application)
    attributes.title = "Compass"
    attributes.contentDescription = "Web browser"
    attributes.keywords = ["browser", "web browser", "web", "internet", "Compass Browser"]
    return CSSearchableItem(uniqueIdentifier: appItemID, domainIdentifier: "app", attributeSet: attributes)
  }
}
