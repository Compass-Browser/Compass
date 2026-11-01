// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import Observation
import WebKit

/// Ad, tracker and cookie-banner blocking: the filter lists downloaded and
/// kept up to date (FilterListUpdater), converted (ABPConverter) and
/// compiled by WebKit. Compiled lists are kept by version, so a launch with
/// nothing new doesn't recompile (that takes 15–30 s per list). Each tab
/// gets the lists its site's settings allow (TabManager).
@MainActor @Observable
final class ContentBlocking {
  struct Status {
    var rules: Int?
    var updated: Date?
    var checked: Date?
    var working = false
    var error: String?
  }

  private(set) var status: [FilterList: Status] = [:]
  private(set) var compiled: [FilterList: WKContentRuleList] = [:]
  /// The compiled lists changed: open tabs pick them up.
  @ObservationIgnored var onChange: (() -> Void)?
  @ObservationIgnored private let updater: FilterListUpdater
  @ObservationIgnored private var updating = false
  private let store = WKContentRuleListStore.default()!

  init() {
    let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
    updater = FilterListUpdater(folder: caches.appendingPathComponent("FilterLists", isDirectory: true))
    for list in FilterList.allCases {
      let state = updater.state(list)
      status[list] = Status(
        rules: UserDefaults.standard.object(forKey: Self.countKey(list)) as? Int,
        updated: state?.updated, checked: state?.checked)
    }
  }

  /// The lists for a site with these settings.
  func lists(blockAds: Bool, blockCookieBanners: Bool) -> [WKContentRuleList] {
    var lists: [WKContentRuleList] = []
    if blockAds { lists += [compiled[.easyList], compiled[.easyPrivacy]].compactMap { $0 } }
    if blockCookieBanners, let cookies = compiled[.cookies] { lists.append(cookies) }
    return lists
  }

  /// At launch and on coming to the foreground: what's compiled already,
  /// then whatever is due for a check (daily), or everything with `force`.
  func refresh(force: Bool = false) async {
    guard !updating else { return }
    updating = true
    defer { updating = false }
    for list in FilterList.allCases where compiled[list] == nil {
      await compile(list)
    }
    for list in FilterList.allCases {
      status[list]?.working = true
      do {
        let changed = try await updater.update(list, force: force)
        status[list]?.error = nil
        if changed || compiled[list] == nil { await compile(list) }
      } catch {
        status[list]?.error = error.localizedDescription
      }
      let state = updater.state(list)
      status[list]?.checked = state?.checked
      status[list]?.updated = state?.updated
      status[list]?.working = false
    }
  }

  /// The downloaded text compiled, or the copy compiled from it before.
  private func compile(_ list: FilterList) async {
    guard let text = updater.text(list), let version = updater.state(list)?.version else { return }
    let id = "\(list.rawValue)-\(version)"
    if let existing = try? await store.contentRuleList(forIdentifier: id) {
      compiled[list] = existing
      onChange?()
      return
    }
    status[list]?.working = true
    let (json, count) = await Task.detached(priority: .utility) { () -> (String?, Int) in
      let result = ABPConverter.convert(text)
      return (try? result.json(), result.rules.count)
    }.value
    guard let json else { return }
    do {
      let ruleList = try await store.compileContentRuleList(forIdentifier: id, encodedContentRuleList: json)
      compiled[list] = ruleList
      status[list]?.rules = count
      UserDefaults.standard.set(count, forKey: Self.countKey(list))
      onChange?()
      // Older versions of this list.
      for old in await store.availableIdentifiers() ?? [] where old.hasPrefix(list.rawValue + "-") && old != id {
        try? await store.removeContentRuleList(forIdentifier: old)
      }
    } catch {
      status[list]?.error = error.localizedDescription
    }
  }

  private static func countKey(_ list: FilterList) -> String { "filterListRules.\(list.rawValue)" }
}
