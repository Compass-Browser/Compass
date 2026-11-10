// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassModel
import CryptoKit
import Foundation
import SyncProto

/// Favicons in COMPASS_SPACES (docs/design/sync.md, "Favicons"): one
/// `CompassFavicon` per host an entry of the account points at, written
/// only from pages this device loaded, read by every device.
extension CompassSpacesBridge {
  /// A larger icon always wins; a different one of the same size only once
  /// the account's is this old.
  static let faviconRefresh: TimeInterval = 7 * 24 * 60 * 60
  /// An icon no entry points at is deleted once it's this old (an initial
  /// download may bring it a batch before its entries).
  static let faviconGrace: TimeInterval = 24 * 60 * 60
  static let faviconMaxBytes = 8 * 1024

  /// The entity's UUID (and client tag) for a host.
  public static func faviconUUID(host: String) -> String {
    var b = Array(SHA256.hash(data: Data(("compass-favicon:" + host.lowercased()).utf8)).prefix(16))
    b[6] = (b[6] & 0x0f) | 0x50
    b[8] = (b[8] & 0x3f) | 0x80
    let hex = b.map { String(format: "%02x", $0) }.joined()
    let i = hex.startIndex
    func part(_ from: Int, _ to: Int) -> Substring {
      hex[hex.index(i, offsetBy: from)..<hex.index(i, offsetBy: to)]
    }
    return [part(0, 8), part(8, 12), part(12, 16), part(16, 20), part(20, 32)].joined(separator: "-")
  }

  static func host(of url: String) -> String? {
    guard let host = URL(string: url)?.host(), !host.isEmpty else { return nil }
    return host.lowercased()
  }

  /// The hosts this account's entries point at, every section included
  /// (not a live folder's entries, which stay on this device).
  @MainActor
  var ownHosts: Set<String> {
    Set(store.allNodes.filter { $0.isTab && !$0.isLiveItem && owns($0) }.compactMap { Self.host(of: $0.url) })
  }

  /// The icons this account has, for the app's cache: (host, PNG).
  @MainActor
  public var syncedFavicons: [(host: String, png: Data)] {
    mirror.values.compactMap { s in
      guard case .favicon(let f) = s.entity, !f.host.isEmpty, !f.png.isEmpty else { return nil }
      return (f.host, f.png)
    }
  }

  /// An icon this device got from a page it loaded. Commits it if an entry
  /// of this account is on the host and it beats the account's (larger, or
  /// different and the account's is a week old). Returns whether it did.
  @MainActor
  @discardableResult
  public func offerFavicon(host: String, png: Data, sourceSize: Int) -> Bool {
    let host = host.lowercased()
    guard tracking, !png.isEmpty, png.count <= Self.faviconMaxBytes, ownHosts.contains(host) else { return false }
    let id = Self.faviconUUID(host: host)
    if case .favicon(let old)? = mirror[id]?.entity {
      let age = now().timeIntervalSince(Self.date(windowsMicros: old.updateTime))
      let larger = sourceSize > Int(old.sourceSize)
      let refresh = sourceSize == Int(old.sourceSize) && png != old.png && age >= Self.faviconRefresh
      guard larger || refresh else { return false }
    }
    var specifics = mirror[id] ?? SyncPb_CompassSpacesSpecifics()
    var f = specifics.favicon
    f.uuid = id
    f.host = host
    f.png = png
    f.sourceSize = Int32(sourceSize)
    f.updateTime = Self.windowsMicros(now())
    specifics.favicon = f
    commit(id, specifics)
    drain()
    return true
  }

  /// Deletes this account's icons no entry points at any more, once past
  /// the grace period.
  @MainActor
  func collectFavicons() {
    guard tracking else { return }
    let hosts = ownHosts
    var collected = false
    for (id, s) in mirror {
      guard case .favicon(let f) = s.entity, !hosts.contains(f.host),
        now().timeIntervalSince(Self.date(windowsMicros: f.updateTime)) > Self.faviconGrace
      else { continue }
      mirror[id] = nil
      lastCommit[id] = nil
      queueDelete(id)
      collected = true
    }
    if collected { drain() }
  }
}
