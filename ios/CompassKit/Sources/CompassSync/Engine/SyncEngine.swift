// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import SwiftProtobuf
import SyncProto

public enum SyncEngineError: Error, Equatable {
  /// The chain has no NIGORI keys yet: no device has started sync on it.
  case chainNotStarted
  /// The passphrase doesn't open the chain's keys (a "Reset sync" elsewhere).
  case crypto(CryptographerError)
  /// The server answered with an error for the whole request.
  case server(Int, String)
  case connection(SyncConnectionError)
}

/// What one cycle did.
public struct SyncReport: Sendable, Equatable {
  public var downloaded: [DataType: Int] = [:]
  public var committed: [DataType: Int] = [:]
  /// Commits the server refused for good (over quota, invalid).
  public var failedCommits: [String] = []
  public var conflicts = 0
}

/// Chromium's sync engine, cut down to what Compass for iOS needs
/// (docs/design/ios.md, "Sync client"): GetUpdates with progress markers,
/// Commit in batches, client-tag entities, the custom-passphrase Nigori,
/// and "remote wins" conflicts (a remote deletion loses to a local edit, as
/// in Chromium's default DataTypeSyncBridge::ResolveConflict).
public actor SyncEngine {
  static let nigoriServerTag = "google_chrome_nigori"
  static let maxGetUpdatesRounds = 100
  static let maxCommitRounds = 3

  let connection: any SyncConnection
  let passphrase: String
  let store: any SyncMetadataStore
  let createChainIfEmpty: Bool
  private(set) var metadata: SyncMetadata
  private var bridges: [DataType: any DataTypeBridge] = [:]
  public private(set) var cryptographer: Cryptographer?
  private var maxCommitBatchSize = 90

  /// `createChainIfEmpty`: write a new NIGORI when the chain has none (the
  /// first device). A joining device leaves it false.
  public init(
    connection: any SyncConnection, passphrase: String, store: any SyncMetadataStore,
    createChainIfEmpty: Bool = false
  ) {
    self.connection = connection
    self.passphrase = passphrase
    self.store = store
    self.createChainIfEmpty = createChainIfEmpty
    metadata = store.load() ?? SyncMetadata()
  }

  public var cacheGUID: String { metadata.cacheGUID }
  public var pollInterval: TimeInterval? { metadata.pollIntervalSeconds.map(TimeInterval.init) }
  public var registeredTypes: [DataType] { Array(bridges.keys) }

  public func register(_ bridge: any DataTypeBridge) async {
    bridges[bridge.dataType] = bridge
    if let cryptographer { await bridge.cryptographerReady(cryptographer) }
  }

  // MARK: Local changes

  /// Queues a local create or update for the next cycle.
  public func put(_ type: DataType, tag: String, specifics: SyncPb_EntitySpecifics) {
    let hash = ClientTagHash.make(type: type, tag: tag)
    var meta = metadata.types[type.rawValue, default: TypeState()].entities[hash]
      ?? EntityMetadata(clientTag: tag)
    meta.pending = PendingChange(specifics: try? specifics.serializedBytes())
    meta.sequence += 1
    metadata.types[type.rawValue, default: TypeState()].entities[hash] = meta
    save()
  }

  /// Queues a local deletion. Unknown or never-committed entities just go.
  public func delete(_ type: DataType, tag: String) {
    let hash = ClientTagHash.make(type: type, tag: tag)
    guard var meta = metadata.types[type.rawValue]?.entities[hash] else { return }
    if meta.serverID == nil {
      metadata.types[type.rawValue]?.entities[hash] = nil
    } else {
      meta.pending = PendingChange(specifics: nil)
      meta.sequence += 1
      metadata.types[type.rawValue]?.entities[hash] = meta
    }
    save()
  }

  public var hasPendingChanges: Bool {
    metadata.types.values.contains { $0.entities.values.contains { $0.pending != nil } }
  }

  /// The client tags this engine knows for a type (tests, debugging).
  public func knownTags(_ type: DataType) -> Set<String> {
    Set(metadata.types[type.rawValue]?.entities.values.map(\.clientTag) ?? [])
  }

  // MARK: Cycle

  /// Downloads every registered type, then commits local changes.
  @discardableResult
  public func sync(origin: SyncPb_SyncEnums.GetUpdatesOrigin = .guTrigger) async throws -> SyncReport {
    var report = SyncReport()
    try await ensureCryptographer()
    try await download(origin: origin, report: &report)
    for _ in 0..<Self.maxCommitRounds {
      let needsRefetch = try await commit(report: &report)
      guard needsRefetch else { break }
      try await download(origin: .guTrigger, report: &report)
    }
    return report
  }

  // MARK: Nigori

  private func ensureCryptographer() async throws {
    if cryptographer != nil { return }
    if let stored = metadata.nigori, let nigori = try? SyncPb_NigoriSpecifics(serializedBytes: stored),
      nigori.passphraseType == 4
    {
      try await adopt(nigori)
      return
    }
    let initial = !(metadata.types[DataType.nigori.rawValue]?.initialSyncDone ?? false)
    var nigoriEntity: SyncPb_SyncEntity?
    try await getUpdates(types: [.nigori], origin: initial ? .newClient : .guTrigger) { entities in
      for e in entities where e.specifics.specificsVariant != nil {
        if case .nigori = e.specifics.specificsVariant { nigoriEntity = e }
      }
    }
    metadata.types[DataType.nigori.rawValue, default: TypeState()].initialSyncDone = true
    guard let entity = nigoriEntity else { throw SyncEngineError.chainNotStarted }
    if entity.specifics.nigori.passphraseType == 4 {
      try await adopt(entity.specifics.nigori)
      return
    }
    guard createChainIfEmpty else { throw SyncEngineError.chainNotStarted }
    let (nigori, cryptographer) = Cryptographer.makeCustomPassphraseNigori(passphrase: passphrase)
    var commit = SyncPb_SyncEntity()
    commit.idString = entity.idString
    commit.parentIDString = entity.parentIDString
    commit.version = entity.version
    commit.name = entity.name
    commit.serverDefinedUniqueTag = Self.nigoriServerTag
    commit.folder = entity.folder
    commit.specifics.nigori = nigori
    let response = try await post(commitMessage([commit]))
    guard response.commit.entryResponse.first?.responseType == .success else {
      // Someone else started the chain meanwhile: read theirs.
      metadata.types[DataType.nigori.rawValue]?.progressToken = nil
      return try await ensureCryptographer()
    }
    metadata.nigori = try nigori.serializedBytes()
    self.cryptographer = cryptographer
    save()
    for bridge in bridges.values { await bridge.cryptographerReady(cryptographer) }
  }

  private func adopt(_ nigori: SyncPb_NigoriSpecifics) async throws {
    do {
      let cryptographer = try Cryptographer(nigori: nigori, passphrase: passphrase)
      self.cryptographer = cryptographer
      metadata.nigori = try nigori.serializedBytes()
      save()
      for bridge in bridges.values { await bridge.cryptographerReady(cryptographer) }
    } catch let error as CryptographerError {
      throw SyncEngineError.crypto(error)
    }
  }

  // MARK: Download

  private func download(origin: SyncPb_SyncEnums.GetUpdatesOrigin, report: inout SyncReport) async throws {
    let types = bridges.keys.sorted { $0.rawValue < $1.rawValue }
    guard !types.isEmpty else { return }
    let initial = types.filter { !(metadata.types[$0.rawValue]?.initialSyncDone ?? false) }
    var changes: [DataType: [RemoteChange]] = [:]
    try await getUpdates(types: types, origin: initial.isEmpty ? origin : .newClient) { entities in
      for entity in entities {
        if let (type, change) = self.process(entity, report: &report) {
          changes[type, default: []].append(change)
        }
      }
    }
    for (type, list) in changes {
      await bridges[type]?.apply(list)
      report.downloaded[type, default: 0] += list.count
    }
    for type in initial {
      guard let bridge = bridges[type] else { continue }
      let known = metadata.types[type.rawValue]?.entities ?? [:]
      for (tag, specifics) in await bridge.localDataForInitialMerge()
      where known[ClientTagHash.make(type: type, tag: tag)] == nil {
        put(type, tag: tag, specifics: specifics)
      }
      metadata.types[type.rawValue, default: TypeState()].initialSyncDone = true
    }
    save()
  }

  /// Folds one downloaded entity into the metadata; returns the change to
  /// apply, or nil (an echo, a server folder, or a local change that wins).
  private func process(_ entity: SyncPb_SyncEntity, report: inout SyncReport) -> (DataType, RemoteChange)? {
    guard entity.serverDefinedUniqueTag.isEmpty, entity.hasClientTagHash,
      let type = DataType(specifics: entity.specifics), type != .nigori, let bridge = bridges[type]
    else { return nil }
    let hash = entity.clientTagHash
    var state = metadata.types[type.rawValue, default: TypeState()]
    var meta = state.entities[hash]
    if let meta, entity.version <= meta.baseVersion { return nil }  // our own commit, or old

    var specifics: SyncPb_EntitySpecifics?
    if !entity.deleted {
      guard let decrypted = cryptographer?.decrypt(specifics: entity.specifics) else { return nil }
      specifics = decrypted
    }

    if let pending = meta?.pending {
      report.conflicts += 1
      if entity.deleted && !pending.isDeletion {
        // Local edit beats a remote deletion: re-create it.
        meta!.serverID = nil
        meta!.baseVersion = 0
        state.entities[hash] = meta
        metadata.types[type.rawValue] = state
        return nil
      }
      meta!.pending = nil  // remote wins
    }

    let change = RemoteChange(
      clientTagHash: hash, specifics: specifics, serverID: entity.idString, version: entity.version,
      mtime: entity.mtime, ctime: entity.ctime, originatorCacheGUID: entity.originatorCacheGuid)
    if entity.deleted {
      state.entities[hash] = nil
    } else {
      var updated = meta ?? EntityMetadata(clientTag: bridge.clientTag(for: specifics!))
      updated.serverID = entity.idString
      updated.baseVersion = entity.version
      updated.serverMtime = entity.mtime
      state.entities[hash] = updated
    }
    metadata.types[type.rawValue] = state
    return (type, change)
  }

  private func getUpdates(
    types: [DataType], origin: SyncPb_SyncEnums.GetUpdatesOrigin,
    handle: ([SyncPb_SyncEntity]) async throws -> Void
  ) async throws {
    for _ in 0..<Self.maxGetUpdatesRounds {
      var gu = SyncPb_GetUpdatesMessage()
      gu.fetchFolders = true
      gu.getUpdatesOrigin = origin
      gu.fromProgressMarker = types.map { type in
        var marker = SyncPb_DataTypeProgressMarker()
        marker.dataTypeID = type.rawValue
        marker.token = metadata.types[type.rawValue]?.progressToken ?? Data()
        return marker
      }
      var message = baseMessage()
      message.messageContents = .getUpdates
      message.getUpdates = gu
      let response = try await post(message)
      try await handle(response.getUpdates.entries)
      for marker in response.getUpdates.newProgressMarker {
        metadata.types[marker.dataTypeID, default: TypeState()].progressToken = marker.token
      }
      save()
      if response.getUpdates.changesRemaining <= 0 { return }
    }
  }

  // MARK: Commit

  /// Returns whether a conflict asks for another download first.
  private func commit(report: inout SyncReport) async throws -> Bool {
    guard let cryptographer else { return false }
    struct Sent {
      let type: DataType
      let hash: String
      let sequence: Int64
      let deletion: Bool
    }
    var queue: [(Sent, SyncPb_SyncEntity)] = []
    let now = Int64(Date().timeIntervalSince1970 * 1000)
    for type in bridges.keys.sorted(by: { $0.rawValue < $1.rawValue }) {
      for (hash, meta) in metadata.types[type.rawValue]?.entities ?? [:] {
        guard let pending = meta.pending else { continue }
        var entity = SyncPb_SyncEntity()
        entity.idString = meta.serverID ?? UUID().uuidString.lowercased()
        entity.version = meta.baseVersion
        entity.clientTagHash = hash
        entity.deleted = pending.isDeletion
        entity.mtime = now
        entity.ctime = now
        if let data = pending.specifics, let specifics = try? SyncPb_EntitySpecifics(serializedBytes: data) {
          if type.isEncrypted {
            entity.specifics = cryptographer.encrypt(specifics: specifics, type: type)
            entity.nonUniqueName = "encrypted"
          } else {
            entity.specifics = specifics
            entity.nonUniqueName = meta.clientTag
          }
        } else {
          entity.specifics = type.emptySpecifics
        }
        queue.append((Sent(type: type, hash: hash, sequence: meta.sequence, deletion: pending.isDeletion), entity))
      }
    }

    var needsRefetch = false
    var index = 0
    while index < queue.count {
      let batch = Array(queue[index..<min(index + maxCommitBatchSize, queue.count)])
      index += batch.count
      let response = try await post(commitMessage(batch.map(\.1)))
      for (i, (sent, _)) in batch.enumerated() {
        guard i < response.commit.entryResponse.count else { break }
        let entry = response.commit.entryResponse[i]
        guard var meta = metadata.types[sent.type.rawValue]?.entities[sent.hash] else { continue }
        switch entry.responseType {
        case .success:
          report.committed[sent.type, default: 0] += 1
          if sent.deletion && meta.sequence == sent.sequence {
            metadata.types[sent.type.rawValue]?.entities[sent.hash] = nil
            continue
          }
          meta.serverID = entry.idString
          meta.baseVersion = entry.version
          meta.serverMtime = entry.mtime
          if meta.sequence == sent.sequence { meta.pending = nil }
        case .conflict:
          needsRefetch = true
        case .overQuota, .invalidMessage:
          report.failedCommits.append("\(sent.type) \(meta.clientTag): \(entry.errorMessage)")
        default:
          break  // retry, transient: try again next cycle
        }
        metadata.types[sent.type.rawValue]?.entities[sent.hash] = meta
      }
      save()
    }
    return needsRefetch
  }

  private func commitMessage(_ entries: [SyncPb_SyncEntity]) -> SyncPb_ClientToServerMessage {
    var commit = SyncPb_CommitMessage()
    commit.entries = entries
    commit.cacheGuid = metadata.cacheGUID
    var message = baseMessage()
    message.messageContents = .commit
    message.commit = commit
    return message
  }

  // MARK: Transport

  private func baseMessage() -> SyncPb_ClientToServerMessage {
    var message = SyncPb_ClientToServerMessage()
    message.share = ""
    if let birthday = metadata.storeBirthday { message.storeBirthday = birthday }
    return message
  }

  private func post(_ message: SyncPb_ClientToServerMessage) async throws -> SyncPb_ClientToServerResponse {
    let response: SyncPb_ClientToServerResponse
    do {
      response = try await connection.post(message)
    } catch let error as SyncConnectionError {
      throw SyncEngineError.connection(error)
    }
    if response.hasStoreBirthday { metadata.storeBirthday = response.storeBirthday }
    if response.hasClientCommand {
      let command = response.clientCommand
      if command.hasSetSyncPollInterval { metadata.pollIntervalSeconds = command.setSyncPollInterval }
      if command.hasMaxCommitBatchSize, command.maxCommitBatchSize > 0 {
        maxCommitBatchSize = Int(command.maxCommitBatchSize)
      }
    }
    if response.hasErrorCode, response.errorCode != .success {
      throw SyncEngineError.server(response.errorCode.rawValue, response.errorMessage)
    }
    return response
  }

  private func save() { store.save(metadata) }
}
