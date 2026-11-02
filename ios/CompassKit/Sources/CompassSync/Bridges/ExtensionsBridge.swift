// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import SyncProto

/// An extension installed on another device (the desktop), from EXTENSIONS.
public struct SyncedExtension: Codable, Sendable, Equatable, Identifiable {
  public var id: String
  public var name: String
  public var version: String
  public var enabled: Bool
  /// Empty or Chrome's update service for Chrome Web Store extensions.
  public var updateURL: String

  public init(id: String, name: String, version: String, enabled: Bool, updateURL: String) {
    self.id = id
    self.name = name
    self.version = version
    self.enabled = enabled
    self.updateURL = updateURL
  }

  /// Whether the Chrome Web Store has it (what the phone can install).
  public var isFromWebStore: Bool {
    updateURL.isEmpty || updateURL.contains("clients2.google.com/service/update2")
      || updateURL.contains("update.googleapis.com")
  }
}

/// EXTENSIONS, download only: the desktop's extension list, so the phone
/// can offer the same ones (docs/design/ios.md, "Web extensions"). The phone
/// never writes it; its extensions run in WebKit, not Chromium. Tag = the
/// extension ID, as Chromium's ExtensionSyncData. The list is mirrored to
/// `stateURL`, since the engine sends each entity only once.
public final class ExtensionsBridge: DataTypeBridge, @unchecked Sendable {
  public let dataType = DataType.extensions

  private let lock = NSLock()
  private var byID: [String: SyncedExtension] = [:]
  private let stateURL: URL?
  /// Called with the list after every change.
  public var onChange: (@Sendable ([SyncedExtension]) -> Void)?

  public init(stateURL: URL?) {
    self.stateURL = stateURL
    if let stateURL, let data = try? Data(contentsOf: stateURL),
      let saved = try? JSONDecoder().decode([SyncedExtension].self, from: data)
    {
      byID = Dictionary(saved.map { ($0.id, $0) }, uniquingKeysWith: { a, _ in a })
    }
  }

  public func clientTag(for specifics: SyncPb_EntitySpecifics) -> String { specifics.extension.id }

  public var extensions: [SyncedExtension] {
    lock.withLock { byID.values.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending } }
  }

  public func apply(_ changes: [RemoteChange]) async {
    let list: [SyncedExtension] = lock.withLock {
      for change in changes {
        if let specifics = change.specifics {
          let e = specifics.extension
          guard !e.id.isEmpty else { continue }
          byID[e.id] = SyncedExtension(
            id: e.id, name: e.name, version: e.version, enabled: e.enabled, updateURL: e.updateURL)
        } else {
          byID = byID.filter { ClientTagHash.make(type: .extensions, tag: $0.key) != change.clientTagHash }
        }
      }
      return byID.values.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }
    if let stateURL, let data = try? JSONEncoder().encode(list) { try? data.write(to: stateURL, options: .atomic) }
    onChange?(list)
  }
}
