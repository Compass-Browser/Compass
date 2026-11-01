// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import SyncProto

/// A tab open on another device.
public struct ForeignTab: Sendable, Equatable, Identifiable {
  public var id: String { "\(sessionTag) \(tabNodeID)" }
  public var sessionTag: String
  public var tabNodeID: Int32
  public var url: String
  public var title: String
  public var lastActive: Date
}

/// Another device's open tabs.
public struct ForeignSession: Sendable, Equatable, Identifiable {
  public var id: String { tag }
  public var tag: String
  public var name: String
  public var formFactor: SyncPb_SyncEnums.DeviceFormFactor
  public var modified: Date
  public var tabs: [ForeignTab]
}

/// SESSIONS, as Chromium's SessionSyncBridge: per device a header entity
/// (tag = the session tag = the device's cache GUID) listing windows and
/// tab IDs, and one entity per tab (tag = "<session tag> <tab node id>").
/// The phone publishes its Today entries as one window, so the Mac's "Tabs
/// on other devices" shows them; and it reads everyone else's.
public final class SessionsBridge: DataTypeBridge, @unchecked Sendable {
  public nonisolated let dataType = DataType.sessions
  /// Sessions not touched for this long are hidden (Chromium's stale cutoff).
  public static let staleAfter: TimeInterval = 14 * 24 * 60 * 60

  public struct LocalTab: Sendable, Equatable {
    public var id: String  // the sidebar node's UUID
    public var url: String
    public var title: String
    public var lastActive: Date
    public init(id: String, url: String, title: String, lastActive: Date) {
      self.id = id
      self.url = url
      self.title = title
      self.lastActive = lastActive
    }
  }

  let engine: SyncEngine
  let deviceName: String
  let stateURL: URL?
  private let lock = NSLock()
  private var headers: [String: SyncPb_SessionHeader] = [:]
  private var modified: [String: Date] = [:]
  private var tabs: [String: [Int32: SyncPb_SessionTab]] = [:]
  private var hashes: [String: (String, Int32)] = [:]  // client tag hash → (session, tab node id)
  /// Our tab node IDs by sidebar node UUID, stable across launches.
  private var nodeIDs: [String: Int32] = [:]
  private var published: [Int32: SyncPb_SessionTab] = [:]
  private var publishedHeader: SyncPb_SessionHeader?
  private var startTime = Int64(Date().timeIntervalSince1970 * 1000)
  public var onChange: (@Sendable ([ForeignSession]) -> Void)?

  struct Saved: Codable {
    var nodeIDs: [String: Int32]
  }

  public init(engine: SyncEngine, deviceName: String, stateURL: URL?) {
    self.engine = engine
    self.deviceName = deviceName
    self.stateURL = stateURL
    if let stateURL, let data = try? Data(contentsOf: stateURL),
      let saved = try? JSONDecoder().decode(Saved.self, from: data)
    {
      nodeIDs = saved.nodeIDs
    }
  }

  public nonisolated func clientTag(for specifics: SyncPb_EntitySpecifics) -> String {
    let s = specifics.session
    if case .header = s.content { return s.sessionTag }
    return "\(s.sessionTag) \(s.tabNodeID)"
  }

  public func apply(_ changes: [RemoteChange]) async {
    let mine = await engine.cacheGUID
    lock.withLock {
      for change in changes {
        if let s = change.specifics?.session {
          guard s.sessionTag != mine, !s.sessionTag.isEmpty else { continue }
          let when = Date(timeIntervalSince1970: TimeInterval(change.mtime) / 1000)
          modified[s.sessionTag] = max(modified[s.sessionTag] ?? .distantPast, when)
          switch s.content {
          case .header(let header):
            headers[s.sessionTag] = header
            hashes[change.clientTagHash] = (s.sessionTag, -1)
          case .tab(let tab):
            tabs[s.sessionTag, default: [:]][s.tabNodeID] = tab
            hashes[change.clientTagHash] = (s.sessionTag, s.tabNodeID)
          default:
            break
          }
        } else if let (tag, node) = hashes.removeValue(forKey: change.clientTagHash) {
          if node < 0 {
            headers[tag] = nil
            tabs[tag] = nil
          } else {
            tabs[tag]?[node] = nil
          }
        }
      }
    }
    onChange?(foreignSessions())
  }

  /// Other devices' sessions with their open tabs (most recent device first).
  public func foreignSessions(now: Date = Date()) -> [ForeignSession] {
    lock.withLock {
      headers.compactMap { tag, header -> ForeignSession? in
        let changed = modified[tag] ?? .distantPast
        guard now.timeIntervalSince(changed) < Self.staleAfter else { return nil }
        let byTabID = Dictionary(
          (tabs[tag] ?? [:]).map { ($0.value.tabID, ($0.key, $0.value)) }, uniquingKeysWith: { a, _ in a })
        let ordered = header.window.flatMap(\.tab).compactMap { byTabID[$0] }
        let list = ordered.compactMap { node, tab -> ForeignTab? in
          guard !tab.navigation.isEmpty else { return nil }
          let index = Int(tab.currentNavigationIndex)
          let nav = tab.navigation[(0..<tab.navigation.count).contains(index) ? index : tab.navigation.count - 1]
          guard !nav.virtualURL.isEmpty else { return nil }
          return ForeignTab(
            sessionTag: tag, tabNodeID: node, url: nav.virtualURL, title: nav.title,
            lastActive: Date(timeIntervalSince1970: TimeInterval(tab.lastActiveTimeUnixEpochMillis) / 1000))
        }
        return ForeignSession(
          tag: tag, name: header.clientName, formFactor: header.deviceFormFactor, modified: changed, tabs: list)
      }.sorted { $0.modified > $1.modified }
    }
  }

  /// Publishes this device's open entries (one window). Only what changed
  /// is committed; tabs that went away are deleted.
  public func publish(_ local: [LocalTab]) async {
    let tag = await engine.cacheGUID
    var puts: [(String, SyncPb_EntitySpecifics)] = []
    var deletes: [String] = []
    lock.withLock {
      var used: Set<Int32> = []
      var windowTabs: [Int32] = []
      for entry in local.prefix(100) {
        let node: Int32
        if let known = nodeIDs[entry.id] {
          node = known
        } else {
          node = (nodeIDs.values.max() ?? 0) + 1
          nodeIDs[entry.id] = node
        }
        used.insert(node)
        windowTabs.append(node)
        var tab = SyncPb_SessionTab()
        tab.tabID = node
        tab.windowID = 1
        tab.tabVisualIndex = Int32(windowTabs.count - 1)
        tab.currentNavigationIndex = 0
        tab.browserType = .typeTabbed
        tab.lastActiveTimeUnixEpochMillis = Int64(entry.lastActive.timeIntervalSince1970 * 1000)
        var nav = SyncPb_TabNavigation()
        nav.virtualURL = entry.url
        nav.title = entry.title
        nav.uniqueID = 1
        nav.timestampMsec = tab.lastActiveTimeUnixEpochMillis
        nav.pageTransition = .link
        tab.navigation = [nav]
        if published[node] != tab {
          published[node] = tab
          var s = SyncPb_EntitySpecifics()
          s.session.sessionTag = tag
          s.session.tabNodeID = node
          s.session.tab = tab
          puts.append(("\(tag) \(node)", s))
        }
      }
      for node in published.keys where !used.contains(node) {
        published[node] = nil
        deletes.append("\(tag) \(node)")
      }
      nodeIDs = nodeIDs.filter { used.contains($0.value) }
      var header = SyncPb_SessionHeader()
      header.clientName = deviceName
      header.deviceFormFactor = .phone
      header.deviceType = .typePhone
      header.sessionStartTimeUnixEpochMillis = startTime
      var window = SyncPb_SessionWindow()
      window.windowID = 1
      window.selectedTabIndex = windowTabs.isEmpty ? -1 : 0
      window.browserType = .typeTabbed
      window.tab = windowTabs
      header.window = [window]
      if publishedHeader != header {
        publishedHeader = header
        var s = SyncPb_EntitySpecifics()
        s.session.sessionTag = tag
        s.session.header = header
        puts.append((tag, s))
      }
    }
    for (t, s) in puts { await engine.put(.sessions, tag: t, specifics: s) }
    for t in deletes { await engine.delete(.sessions, tag: t) }
    if let stateURL, let data = try? JSONEncoder().encode(Saved(nodeIDs: lock.withLock { nodeIDs })) {
      try? data.write(to: stateURL, options: .atomic)
    }
  }
}
