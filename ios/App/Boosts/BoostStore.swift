// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import Foundation
import Observation

/// The Boosts (docs/design/boosts.md): per-site CSS and JavaScript, in
/// Application Support/Compass/Boosts.json, synced with the home account's
/// spaces (CompassSpacesBridge+Boosts) and applied to every non-Incognito
/// page by BoostInjector. The rules are CompassBoosts' `BoostList`, the
/// desktop's.
@MainActor
@Observable
final class BoostStore {
  static let shared = BoostStore()

  private(set) var list: BoostList
  var boosts: [Boost] { list.boosts }

  /// After every change, local or remote: sync commits what differs.
  @ObservationIgnored var onChange: (([Boost]) -> Void)?

  @ObservationIgnored private let url: URL

  private init() {
    let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Compass", isDirectory: true)
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    url = directory.appendingPathComponent("Boosts.json")
    list = (try? Data(contentsOf: url)).flatMap(BoostList.decode) ?? BoostList()
  }

  func find(_ id: String) -> Boost? { list.find(id) }

  /// The enabled Boosts for a page.
  func matching(_ url: URL?) -> [Boost] {
    guard let url else { return [] }
    return list.matching(url)
  }

  /// What `draft` would make (not saved): see `BoostList.resolve`.
  func resolve(_ draft: BoostDraft, author: Boost.Author) -> Result<Boost, BoostList.Failure> {
    list.resolve(draft, author: author)
  }

  /// Creates or edits a Boost by hand (the editor).
  func save(_ draft: BoostDraft) -> Result<Boost, BoostList.Failure> {
    let result = list.resolve(draft, author: .user)
    if case .success(let boost) = result, !put(boost) { return .failure(.full) }
    return result
  }

  /// Adds or replaces `boost` as it is; false if it can't be kept.
  @discardableResult
  func put(_ boost: Boost) -> Bool {
    if list.find(boost.id) == boost { return true }
    guard list.put(boost) else { return false }
    changed()
    return true
  }

  func setEnabled(_ id: String, _ enabled: Bool) {
    guard var boost = list.find(id), boost.enabled != enabled else { return }
    boost.enabled = enabled
    boost.updateTime = max(Boost.windowsMicros(Date()), boost.updateTime + 1)
    put(boost)
  }

  @discardableResult
  func remove(_ id: String) -> Boost? {
    let removed = list.remove(id)
    if removed != nil { changed() }
    return removed
  }

  /// Puts `boost` back as it was (Undo).
  @discardableResult
  func restore(_ boost: Boost) -> Bool {
    let restored = list.restore(boost)
    if restored { changed() }
    return restored
  }

  func applyRemote(_ upserts: [Boost], deleted: Set<String>) {
    if list.applyRemote(upserts, deleted: deleted) { changed() }
  }

  private func changed() {
    try? list.encoded().write(to: url, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    BoostInjector.shared.update(list.boosts)
    onChange?(list.boosts)
  }
}
