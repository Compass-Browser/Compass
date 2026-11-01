// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// What the engine remembers about one entity (Chromium's EntityMetadata).
public struct EntityMetadata: Codable, Sendable, Equatable {
  /// The server's ID; nil until the first commit succeeds.
  public var serverID: String?
  /// The server version we last saw or committed; 0 = never committed.
  public var baseVersion: Int64 = 0
  public var clientTag: String
  /// A local change waiting to be committed.
  public var pending: PendingChange?
  /// Bumped on every local change, so a commit only clears the change it sent.
  public var sequence: Int64 = 0
  public var serverMtime: Int64 = 0
}

public struct PendingChange: Codable, Sendable, Equatable {
  /// Serialized EntitySpecifics (plaintext); nil for a deletion.
  public var specifics: Data?
  public var isDeletion: Bool { specifics == nil }
}

public struct TypeState: Codable, Sendable, Equatable {
  public var progressToken: Data?
  public var initialSyncDone = false
  /// Keyed by client tag hash.
  public var entities: [String: EntityMetadata] = [:]
}

/// Everything the engine persists between launches, saved as one file.
public struct SyncMetadata: Codable, Sendable, Equatable {
  public var cacheGUID: String
  public var storeBirthday: String?
  /// The serialized NIGORI entity's specifics, once known.
  public var nigori: Data?
  public var pollIntervalSeconds: Int32?
  public var types: [Int32: TypeState] = [:]

  public init(cacheGUID: String = SyncMetadata.makeCacheGUID()) {
    self.cacheGUID = cacheGUID
  }

  /// Chromium's cache GUIDs: 16 random bytes, base64.
  public static func makeCacheGUID() -> String {
    var bytes = [UInt8](repeating: 0, count: 16)
    precondition(SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes) == errSecSuccess)
    return Data(bytes).base64EncodedString()
  }
}

/// Where SyncMetadata lives. The default is a file; tests use memory.
public protocol SyncMetadataStore: Sendable {
  func load() -> SyncMetadata?
  func save(_ metadata: SyncMetadata)
}

public struct FileSyncMetadataStore: SyncMetadataStore {
  let url: URL
  public init(url: URL) { self.url = url }

  public func load() -> SyncMetadata? {
    guard let data = try? Data(contentsOf: url) else { return nil }
    return try? PropertyListDecoder().decode(SyncMetadata.self, from: data)
  }

  public func save(_ metadata: SyncMetadata) {
    let encoder = PropertyListEncoder()
    encoder.outputFormat = .binary
    guard let data = try? encoder.encode(metadata) else { return }
    try? FileManager.default.createDirectory(
      at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try? data.write(to: url, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
  }
}

public final class MemorySyncMetadataStore: SyncMetadataStore, @unchecked Sendable {
  private let lock = NSLock()
  private var value: SyncMetadata?
  public init(_ value: SyncMetadata? = nil) { self.value = value }
  public func load() -> SyncMetadata? { lock.withLock { value } }
  public func save(_ metadata: SyncMetadata) { lock.withLock { value = metadata } }
}
