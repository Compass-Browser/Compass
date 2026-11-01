// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import Foundation
import SyncProto

/// HISTORY, as Chromium's HistorySyncBridge: one entity per visit, client
/// tag = the visit time (Windows-epoch µs) as a decimal string. Each device
/// commits only its own visits; others' arrive with their cache GUID.
/// Required by the desktop: a visit time, the originator, at least one
/// redirect entry (GetSpecificsError in history_sync_bridge.cc).
public final class HistoryBridge: DataTypeBridge, @unchecked Sendable {
  public nonisolated let dataType = DataType.history
  @MainActor let history: HistoryStore
  let engine: SyncEngine
  /// The profile account (empty: the home's); its visits are tagged with it.
  let profile: String
  @MainActor private var lastTime: Int64 = 0

  @MainActor
  public init(history: HistoryStore, engine: SyncEngine, profile: String = "") {
    self.history = history
    self.engine = engine
    self.profile = profile
  }

  public nonisolated func clientTag(for specifics: SyncPb_EntitySpecifics) -> String {
    String(specifics.history.visitTimeWindowsEpochMicros)
  }

  public nonisolated func apply(_ changes: [RemoteChange]) async {
    let mine = await engine.cacheGUID
    await MainActor.run {
      for change in changes {
        guard let h = change.specifics?.history, h.originatorCacheGuid != mine, h.visitTimeWindowsEpochMicros > 0,
          let last = h.redirectEntries.last, !last.url.isEmpty
        else { continue }
        history.add(
          Visit(
            time: h.visitTimeWindowsEpochMicros, url: last.url, title: last.title, originator: h.originatorCacheGuid,
            profile: profile))
      }
    }
  }

  /// Records a visit of this device and queues it for sync. Returns its time
  /// (pass it back with the title once the page has one).
  @MainActor
  @discardableResult
  public func record(url: String, title: String, typed: Bool = false, at date: Date = Date(), time: Int64? = nil) async
    -> Int64?
  {
    guard Self.shouldSync(url) else { return nil }
    let guid = await engine.cacheGUID
    var t = time ?? Visit.time(date)
    if time == nil {
      // Unique per device: two visits in the same µs would share a tag.
      t = max(t, lastTime + 1)
      lastTime = t
    }
    history.add(Visit(time: t, url: url, title: title, originator: guid, profile: profile))
    var h = SyncPb_HistorySpecifics()
    h.visitTimeWindowsEpochMicros = t
    h.originatorCacheGuid = guid
    var entry = SyncPb_HistorySpecifics.RedirectEntry()
    entry.url = url
    if !title.isEmpty { entry.title = title }
    entry.originatorVisitID = t
    h.redirectEntries = [entry]
    var transition = SyncPb_HistorySpecifics.PageTransition()
    transition.coreTransition = typed ? .typed : .link
    transition.fromAddressBar = typed
    h.pageTransition = transition
    h.browserType = .typeTabbed
    var specifics = SyncPb_EntitySpecifics()
    specifics.history = h
    await engine.put(.history, tag: String(t), specifics: specifics)
    return t
  }

  /// Chromium's ShouldSync: web pages only, and no data: URLs.
  static func shouldSync(_ url: String) -> Bool {
    guard let scheme = URL(string: url)?.scheme?.lowercased() else { return false }
    return scheme == "http" || scheme == "https"
  }
}

/// HISTORY_DELETE_DIRECTIVES: "delete these visits" messages. Received ones
/// delete local history; "Clear History" here sends a time range one.
/// Times in these are µs since the *Unix* epoch.
public final class HistoryDeleteDirectivesBridge: DataTypeBridge, @unchecked Sendable {
  public nonisolated let dataType = DataType.historyDeleteDirectives
  @MainActor let history: HistoryStore
  let engine: SyncEngine
  /// The profile account (empty: the home's): directives delete its visits.
  let profile: String
  static let windowsOffset: Int64 = 11_644_473_600_000_000

  @MainActor
  public init(history: HistoryStore, engine: SyncEngine, profile: String = "") {
    self.history = history
    self.engine = engine
    self.profile = profile
  }

  /// Chromium's directives have random tags that aren't in the specifics.
  public nonisolated func clientTag(for specifics: SyncPb_EntitySpecifics) -> String { "" }

  public nonisolated func apply(_ changes: [RemoteChange]) async {
    await MainActor.run {
      for change in changes {
        guard let d = change.specifics?.historyDeleteDirective else { continue }
        apply(d)
      }
    }
  }

  @MainActor
  func apply(_ d: SyncPb_HistoryDeleteDirectiveSpecifics) {
    switch d.directiveType {
    case .timeRangeDirective(let r):
      history.delete(
        from: r.startTimeUsec + Self.windowsOffset, to: r.endTimeUsec + Self.windowsOffset, profile: profile)
    case .urlDirective(let u):
      history.delete(url: u.url, until: u.endTimeUsec + Self.windowsOffset, profile: profile)
    case .globalIDDirective(let g):
      // Global IDs are visit times (Windows-epoch µs).
      history.delete(times: g.globalID, profile: profile)
    case nil:
      break
    }
  }

  /// Deletes history between two dates here and on every device.
  @MainActor
  public func deleteHistory(from start: Date, to end: Date) async {
    var d = SyncPb_HistoryDeleteDirectiveSpecifics()
    d.timeRangeDirective.startTimeUsec = Int64(start.timeIntervalSince1970 * 1_000_000)
    d.timeRangeDirective.endTimeUsec = Int64(end.timeIntervalSince1970 * 1_000_000)
    apply(d)
    var specifics = SyncPb_EntitySpecifics()
    specifics.historyDeleteDirective = d
    let alphabet = Array("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
    let tag = String((0..<8).map { _ in alphabet.randomElement()! })
    await engine.put(.historyDeleteDirectives, tag: tag, specifics: specifics)
  }
}
