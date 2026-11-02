// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import SyncProto

/// One change from the server, decrypted.
public struct RemoteChange: Sendable {
  public var clientTagHash: String
  /// nil for a deletion.
  public var specifics: SyncPb_EntitySpecifics?
  public var serverID: String
  public var version: Int64
  public var mtime: Int64
  public var ctime: Int64
  /// The device that created it (its cache GUID).
  public var originatorCacheGUID: String

  public var isDeletion: Bool { specifics == nil }
}

/// A model that syncs one data type (Chromium's DataTypeSyncBridge). The
/// engine calls it on its own actor; local changes go the other way through
/// SyncEngine.put/delete.
public protocol DataTypeBridge: AnyObject, Sendable {
  var dataType: DataType { get }

  /// The client tag of an entity, from its specifics (e.g. a sidebar
  /// node's UUID). It must be what the desktop uses.
  func clientTag(for specifics: SyncPb_EntitySpecifics) -> String

  /// Applies remote changes to the model. Must not echo them back as local
  /// changes.
  func apply(_ changes: [RemoteChange]) async

  /// After the type's first download: the local entities to merge into the
  /// chain (those the server doesn't have get committed). Default: none.
  func localDataForInitialMerge() async -> [(tag: String, specifics: SyncPb_EntitySpecifics)]

  /// The engine has the chain's keys (types with nested encryption, i.e.
  /// passwords, need them).
  func cryptographerReady(_ cryptographer: Cryptographer) async
}

extension DataTypeBridge {
  public func localDataForInitialMerge() async -> [(tag: String, specifics: SyncPb_EntitySpecifics)] { [] }
  public func cryptographerReady(_ cryptographer: Cryptographer) async {}
}

extension DataType {
  /// The type of an entity from its specifics (encrypted ones keep their
  /// type's empty field).
  init?(specifics: SyncPb_EntitySpecifics) {
    switch specifics.specificsVariant {
    case .nigori: self = .nigori
    case .deviceInfo: self = .deviceInfo
    case .compassSpaces: self = .compassSpaces
    case .password: self = .passwords
    case .history: self = .history
    case .historyDeleteDirective: self = .historyDeleteDirectives
    case .session: self = .sessions
    case .extension: self = .extensions
    default: return nil
    }
  }

  /// Compass encrypts every type with the chain's passphrase except these
  /// two, as the desktop does (sync.md; history is encrypted too, "Slice 2b").
  var isEncrypted: Bool { self != .nigori && self != .deviceInfo }

  /// Whether the engine keeps entity metadata after a commit. Chromium's
  /// history types don't: each device only writes its own visits, the server
  /// treats a re-commit of a tag as an update, and keeping 30k entries'
  /// metadata would only cost memory.
  var tracksEntities: Bool { self != .history && self != .historyDeleteDirectives }
}
