// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import SyncProto

/// A tab another device sent to this phone (SEND_TAB_TO_SELF).
public struct SentTab: Sendable, Equatable, Identifiable {
  public var id: String { guid }
  public var guid: String
  public var url: String
  public var title: String
  /// The sender's name, as it put it in the entry.
  public var deviceName: String
  public var sharedTime: Date
}

/// SEND_TAB_TO_SELF (docs/design/send-tab.md): the desktop's "Send to your
/// devices" writes an entry naming this phone's cache GUID; the phone shows
/// it as a notification once, and marks it opened when it's tapped, as
/// Chromium's SendTabToSelfBridge does. Tag = the entry's GUID. The GUIDs
/// already shown are kept in `stateURL`, since a cycle may bring an entry
/// again (another device marks it, a fresh download).
public final class SendTabBridge: DataTypeBridge, @unchecked Sendable {
  /// Chromium's kExpiryTime: older entries aren't shown.
  public static let expiry: TimeInterval = 10 * 24 * 60 * 60
  public let dataType = DataType.sendTabToSelf

  private let lock = NSLock()
  private let engine: SyncEngine
  private let cacheGUID: String
  private let stateURL: URL?
  private var entries: [String: SyncPb_SendTabToSelfSpecifics] = [:]
  private var shown: Set<String> = []
  /// `shown`, oldest first (what's saved).
  private var shownOrder: [String] = []
  static let keep = 500
  /// New tabs for this phone, after every cycle that brings some.
  public var onReceived: (@Sendable ([SentTab]) -> Void)?

  public init(engine: SyncEngine, cacheGUID: String, stateURL: URL?) {
    self.engine = engine
    self.cacheGUID = cacheGUID
    self.stateURL = stateURL
    if let stateURL, let data = try? Data(contentsOf: stateURL),
      let saved = try? JSONDecoder().decode([String].self, from: data)
    {
      shownOrder = saved
      shown = Set(saved)
    }
  }

  public func clientTag(for specifics: SyncPb_EntitySpecifics) -> String { specifics.sendTabToSelf.guid }

  public func apply(_ changes: [RemoteChange]) async {
    let now = Date()
    let (received, state): ([SentTab], [String]?) = lock.withLock {
      var received: [SentTab] = []
      for change in changes {
        guard let specifics = change.specifics else {
          entries = entries.filter { ClientTagHash.make(type: .sendTabToSelf, tag: $0.key) != change.clientTagHash }
          continue
        }
        let entry = specifics.sendTabToSelf
        guard !entry.guid.isEmpty else { continue }
        entries[entry.guid] = entry
        let shared = Self.date(protoTime: entry.sharedTimeUsec)
        guard entry.targetDeviceSyncCacheGuid == cacheGUID, !entry.opened, !entry.notificationDismissed,
          !shown.contains(entry.guid), now.timeIntervalSince(shared) < Self.expiry,
          let url = URL(string: entry.url), url.scheme == "http" || url.scheme == "https"
        else { continue }
        shown.insert(entry.guid)
        shownOrder.append(entry.guid)
        received.append(
          SentTab(guid: entry.guid, url: entry.url, title: entry.title, deviceName: entry.deviceName, sharedTime: shared))
      }
      guard !received.isEmpty else { return (received, nil) }
      // The newest few hundred are plenty: older entries have expired.
      if shownOrder.count > Self.keep {
        for guid in shownOrder.prefix(shownOrder.count - Self.keep) { shown.remove(guid) }
        shownOrder.removeFirst(shownOrder.count - Self.keep)
      }
      return (received, shownOrder)
    }
    if let state, let stateURL, let data = try? JSONEncoder().encode(state) {
      try? data.write(to: stateURL, options: .atomic)
    }
    if !received.isEmpty { onReceived?(received.sorted { $0.sharedTime < $1.sharedTime }) }
  }

  /// A proto time: microseconds since the Windows epoch, as Chromium
  /// writes them (send_tab_to_self_entry.cc, `TimeToProtoTime`).
  public static func protoTime(_ date: Date) -> Int64 {
    Int64((date.timeIntervalSince1970 * 1_000_000).rounded(.down)) + windowsEpochOffset
  }

  /// A proto time read back. Entries counted from 1970 (as the phone's
  /// tests wrote them) are too small for the Windows epoch, and taken as
  /// such.
  public static func date(protoTime: Int64) -> Date {
    let unix = protoTime >= windowsEpochOffset ? protoTime - windowsEpochOffset : protoTime
    return Date(timeIntervalSince1970: TimeInterval(unix) / 1_000_000)
  }

  /// Microseconds from 1601 to 1970.
  static let windowsEpochOffset: Int64 = 11_644_473_600_000_000

  /// The entry Chromium's SendTabToSelfModel::SendEntry() writes: this
  /// device's name as the sender, the target's cache GUID.
  public static func entry(
    guid: String, url: String, title: String, deviceName: String, target: String, now: Date = Date()
  ) -> SyncPb_SendTabToSelfSpecifics {
    var entry = SyncPb_SendTabToSelfSpecifics()
    entry.guid = guid
    entry.url = url
    entry.title = title
    entry.sharedTimeUsec = protoTime(now)
    entry.deviceName = deviceName
    entry.targetDeviceSyncCacheGuid = target
    return entry
  }

  /// Sends a tab to another device of the account (Claude's
  /// send_tab_to_device): it shows there as a notification. Returns the
  /// entry's GUID.
  @discardableResult
  public func send(url: String, title: String, deviceName: String, to target: String, now: Date = Date()) async -> String {
    let guid = UUID().uuidString.lowercased()
    let entry = Self.entry(guid: guid, url: url, title: title, deviceName: deviceName, target: target, now: now)
    lock.withLock { entries[guid] = entry }
    var specifics = SyncPb_EntitySpecifics()
    specifics.sendTabToSelf = entry
    await engine.put(.sendTabToSelf, tag: guid, specifics: specifics)
    return guid
  }

  /// The tab was opened from its notification: the entry says so, and the
  /// sender's list (and other devices) stop offering it.
  public func markOpened(_ guid: String) async {
    let specifics: SyncPb_EntitySpecifics? = lock.withLock {
      guard var entry = entries[guid] else { return nil }
      entry.opened = true
      entry.notificationDismissed = true
      entries[guid] = entry
      var s = SyncPb_EntitySpecifics()
      s.sendTabToSelf = entry
      return s
    }
    guard let specifics else { return }
    await engine.put(.sendTabToSelf, tag: guid, specifics: specifics)
  }
}
