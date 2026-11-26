// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Observation
import SwiftProtobuf

/// Every board, object and image, as sync entities (images without their
/// bytes: `EaselStore.assetData` has them).
public struct EaselEntities: Sendable, Equatable {
  public var easels: [EaselBoard] = []
  public var objects: [EaselObject] = []
  public var assets: [EaselAsset] = []
  public init(easels: [EaselBoard] = [], objects: [EaselObject] = [], assets: [EaselAsset] = []) {
    self.easels = easels
    self.objects = objects
    self.assets = assets
  }
  public var isEmpty: Bool { easels.isEmpty && objects.isEmpty && assets.isEmpty }
}

/// The phone's Easels (docs/design/easels.md), the desktop's EaselStore
/// (browser/easels/easel_store.cc) with its rules and file format: boards
/// and objects in `<directory>/easels.pb`, records of a varint length and a
/// CompassSpacesSpecifics (an image's without its bytes), images in
/// `<directory>/assets/<sha256>`. Synced with the home account
/// (CompassSpacesBridge+Easels).
///
/// Unlike the desktop's, it reads the file at once (it's small) and keeps
/// images on disk, reading one when it's shown.
@MainActor
@Observable
public final class EaselStore {
  public static let fileName = "easels.pb"
  public static let assetsDirectory = "assets"

  private(set) var easelsByID: [String: EaselBoard] = [:]
  private(set) var objectsByID: [String: EaselObject] = [:]
  /// By SHA-256, without their bytes.
  private(set) var assetsBySHA: [String: EaselAsset] = [:]
  @ObservationIgnored private var assetSizes: [String: Int] = [:]
  /// Images' bytes: all of them without a directory (tests), else those
  /// read lately.
  @ObservationIgnored private var assetBytes: [String: Data] = [:]
  @ObservationIgnored private let directory: URL?
  @ObservationIgnored private var saveTask: Task<Void, Never>?
  /// After every change, local or remote: sync commits what differs.
  @ObservationIgnored public var onChange: (() -> Void)?
  /// Also called after every change, after `onChange`: rooms, which sync
  /// shared boards (CompassRooms).
  @ObservationIgnored public var changeObservers: [() -> Void] = []
  @ObservationIgnored public var now: () -> Date = Date.init

  /// `directory` nil: in memory only (tests).
  public init(directory: URL?, now: @escaping () -> Date = Date.init) {
    self.directory = directory
    self.now = now
    if let directory { load(directory) }
    dropUnusedAssets()
  }

  // MARK: Reading

  /// Boards, newest first (create time).
  public var easels: [EaselBoard] {
    easelsByID.values.sorted { $0.createTime != $1.createTime ? $0.createTime > $1.createTime : $0.uuid < $1.uuid }
  }

  public func easel(_ uuid: String) -> EaselBoard? { easelsByID[uuid.lowercased()] }

  /// A board's objects, lowest z first.
  public func objects(of easel: String) -> [EaselObject] {
    let key = easel.lowercased()
    return objectsByID.values.filter { $0.easelUuid == key }
      .sorted { $0.z != $1.z ? $0.z < $1.z : ($0.createTime != $1.createTime ? $0.createTime < $1.createTime : $0.uuid < $1.uuid) }
  }

  public func objectCount(of easel: String) -> Int {
    let key = easel.lowercased()
    return objectsByID.values.reduce(0) { $0 + ($1.easelUuid == key ? 1 : 0) }
  }

  public func object(_ uuid: String) -> EaselObject? { objectsByID[uuid.lowercased()] }

  /// An image's type and times, by SHA-256.
  public func asset(_ sha256: String) -> EaselAsset? { assetsBySHA[sha256] }

  /// An image's bytes, or nil if it isn't here (or its file is damaged).
  public func assetData(_ sha256: String) -> Data? {
    guard assetsBySHA[sha256] != nil else { return nil }
    if let data = assetBytes[sha256] { return data }
    guard let url = assetURL(sha256), let data = try? Data(contentsOf: url), Easels.sha256Hex(data) == sha256 else {
      return nil
    }
    // A few recent ones; the editor keeps its own decoded images.
    if assetBytes.count > 32 { assetBytes.removeAll() }
    assetBytes[sha256] = data
    return data
  }

  // MARK: Boards

  /// A new board (`uuid` nil: a random one). Fails when there are 200.
  public func createEasel(title: String, uuid: String? = nil) -> Result<EaselBoard, EaselError> {
    guard easelsByID.count < EaselLimits.maxEasels else {
      return .failure(EaselError(String(localized: "There are already 200 Easels. Delete one that's no longer needed first.", bundle: .module, comment: "Easel error, shown in an alert or toast.")))
    }
    let id = uuid?.lowercased() ?? UUID().uuidString.lowercased()
    guard Easels.isLowerUUID(id), easelsByID[id] == nil else { return .failure(EaselError(String(localized: "That Easel already exists.", bundle: .module, comment: "Easel error, shown in an alert or toast."))) }
    var easel = EaselBoard()
    easel.uuid = id
    easel.title = Easels.trimmedTitle(title)
    let time = Easels.syncTime(now())
    easel.createTime = time
    easel.updateTime = time
    easelsByID[id] = easel
    changed()
    return .success(easel)
  }

  @discardableResult
  public func renameEasel(_ uuid: String, to title: String) -> Bool {
    guard var easel = easelsByID[uuid.lowercased()] else { return false }
    let title = Easels.trimmedTitle(title)
    if easel.title != title {
      easel.title = title
      easel.updateTime = max(Easels.syncTime(now()), easel.updateTime + 1)
      easelsByID[easel.uuid] = easel
      changed()
    }
    return true
  }

  @discardableResult
  public func setPalette(_ uuid: String, _ palette: String) -> Bool {
    guard var easel = easelsByID[uuid.lowercased()] else { return false }
    if easel.palette != palette {
      easel.palette = String(palette.prefix(32))
      easel.updateTime = max(Easels.syncTime(now()), easel.updateTime + 1)
      easelsByID[easel.uuid] = easel
      changed()
    }
    return true
  }

  /// Deletes the board and its objects. Its images stay until the next
  /// start's clean-up, so Undo can bring objects back.
  @discardableResult
  public func deleteEasel(_ uuid: String) -> Bool {
    let key = uuid.lowercased()
    guard easelsByID.removeValue(forKey: key) != nil else { return false }
    objectsByID = objectsByID.filter { $0.value.easelUuid != key }
    changed()
    return true
  }

  // MARK: Objects

  /// Adds `object` (no uuid: a new one) or replaces the one with its uuid,
  /// which keeps its create time. Its board must exist.
  public func save(_ object: EaselObject) -> Result<EaselObject, EaselError> {
    save([object]).map { $0[0] }
  }

  /// Several at once (a drag of a selection), all or nothing.
  public func save(_ objects: [EaselObject]) -> Result<[EaselObject], EaselError> {
    var normalized: [EaselObject] = []
    var added: [String: Int] = [:]
    for object in objects {
      let checked: EaselObject
      switch Easels.normalize(object) {
      case .success(let o): checked = o
      case .failure(let e): return .failure(e)
      }
      guard easelsByID[checked.easelUuid] != nil else { return .failure(EaselError(String(localized: "There's no such Easel.", bundle: .module, comment: "Easel error, shown in an alert or toast."))) }
      if checked.uuid.isEmpty || objectsByID[checked.uuid] == nil {
        if !checked.uuid.isEmpty && !Easels.isLowerUUID(checked.uuid) {
          return .failure(EaselError("An object's id must be a UUID."))
        }
        added[checked.easelUuid, default: 0] += 1
        if objectCount(of: checked.easelUuid) + added[checked.easelUuid]! > EaselLimits.maxObjectsPerEasel {
          return .failure(EaselError(String(localized: "This Easel is full (1,000 objects). Delete some first.", bundle: .module, comment: "Easel error, shown in an alert or toast.")))
        }
      }
      for sha in Easels.assets(of: checked) where assetsBySHA[sha] == nil {
        return .failure(EaselError(String(localized: "An image of the object is missing.", bundle: .module, comment: "Easel error, shown in an alert or toast.")))
      }
      normalized.append(checked)
    }
    let time = Easels.syncTime(now())
    var any = false
    for i in normalized.indices {
      if normalized[i].uuid.isEmpty { normalized[i].uuid = UUID().uuidString.lowercased() }
      var o = normalized[i]
      if let old = objectsByID[o.uuid] {
        o.createTime = old.createTime
        o.updateTime = old.updateTime
        if o == old {
          normalized[i] = o
          continue
        }
        o.updateTime = max(time, old.updateTime + 1)
      } else {
        o.createTime = time
        o.updateTime = time
      }
      normalized[i] = o
      objectsByID[o.uuid] = o
      any = true
    }
    if any { changed() }
    return .success(normalized)
  }

  @discardableResult
  public func deleteObject(_ uuid: String) -> Bool {
    guard objectsByID.removeValue(forKey: uuid.lowercased()) != nil else { return false }
    changed()
    return true
  }

  // MARK: Images

  /// Keeps an image already fitted (`EaselImages.fit`): at most 512 KiB
  /// and 2048 px a side. Returns its SHA-256; one already here is kept once.
  public func addAsset(_ data: Data, mimeType: String) -> Result<String, EaselError> {
    guard !data.isEmpty, data.count <= EaselLimits.maxAssetBytes else {
      return .failure(EaselError(String(localized: "The image is too large.", bundle: .module, comment: "Easel error, shown in an alert or toast.")))
    }
    guard Easels.isKnownMime(mimeType) else { return .failure(EaselError(String(localized: "Only PNG, JPEG and WebP images can be added.", bundle: .module, comment: "Easel error, shown in an alert or toast."))) }
    let sha = Easels.sha256Hex(data)
    if assetsBySHA[sha] != nil { return .success(sha) }
    guard assetSizes.values.reduce(0, +) + data.count <= EaselLimits.maxAssetBytesTotal else {
      return .failure(EaselError(String(localized: "Easels hold 64 MB of images, which is used up. Delete some images first.", bundle: .module, comment: "Easel error, shown in an alert or toast.")))
    }
    var asset = EaselAsset()
    asset.uuid = Easels.assetUUID(sha256: sha)
    asset.sha256 = sha
    asset.mimeType = mimeType
    asset.createTime = Easels.syncTime(now())
    keep(asset, data: data)
    changed()
    return .success(sha)
  }

  private func keep(_ asset: EaselAsset, data: Data) {
    var meta = asset
    meta.clearData()
    assetsBySHA[asset.sha256] = meta
    assetSizes[asset.sha256] = data.count
    if let url = assetURL(asset.sha256) {
      try? FileManager.default.createDirectory(
        at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
      if !FileManager.default.fileExists(atPath: url.path) { try? data.write(to: url, options: .atomic) }
    } else {
      assetBytes[asset.sha256] = data
    }
  }

  // MARK: Sync

  /// Everything, for sync: images without their bytes.
  public func entities() -> EaselEntities {
    EaselEntities(
      easels: easelsByID.keys.sorted().map { easelsByID[$0]! },
      objects: objectsByID.keys.sorted().map { objectsByID[$0]! },
      assets: assetsBySHA.keys.sorted().map { assetsBySHA[$0]! })
  }

  /// What another device changed: `changed` replace or add by uuid unless
  /// the one here was edited later (images are kept once, they never
  /// change); `deleted` are client tags of any of the three kinds.
  /// `force`: replace even what was edited here later (a shared board's
  /// viewer, whose edits don't count).
  public func applyRemote(_ changed: EaselEntities, deleted: Set<String>, force: Bool = false) {
    var any = false
    for easel in changed.easels where Easels.isLowerUUID(easel.uuid) {
      if let old = easelsByID[easel.uuid], (!force && easel.updateTime < old.updateTime) || easel == old { continue }
      easelsByID[easel.uuid] = easel
      any = true
    }
    for object in changed.objects where Easels.isLowerUUID(object.uuid) {
      if let old = objectsByID[object.uuid], (!force && object.updateTime < old.updateTime) || object == old { continue }
      objectsByID[object.uuid] = object
      any = true
    }
    for asset in changed.assets {
      // One whose bytes don't match its hash, or that's too large, is left
      // out.
      guard Easels.isSha256Hex(asset.sha256), assetsBySHA[asset.sha256] == nil,
        asset.data.count <= EaselLimits.maxAssetBytes, Easels.sha256Hex(asset.data) == asset.sha256
      else { continue }
      keep(asset, data: asset.data)
      any = true
    }
    if !deleted.isEmpty {
      let before = easelsByID.count + objectsByID.count + assetsBySHA.count
      easelsByID = easelsByID.filter { !deleted.contains($0.key) }
      objectsByID = objectsByID.filter { !deleted.contains($0.key) }
      let gone = assetsBySHA.values.filter { deleted.contains($0.uuid) }.map(\.sha256)
      forget(assets: gone)
      any = any || easelsByID.count + objectsByID.count + assetsBySHA.count != before
    }
    if any { self.changed() }
  }

  // MARK: Files

  private func assetURL(_ sha256: String) -> URL? {
    directory?.appendingPathComponent(Self.assetsDirectory, isDirectory: true).appendingPathComponent(sha256)
  }

  private func forget(assets shas: [String]) {
    for sha in shas {
      assetsBySHA[sha] = nil
      assetSizes[sha] = nil
      assetBytes[sha] = nil
      if let url = assetURL(sha) { try? FileManager.default.removeItem(at: url) }
    }
  }

  /// At load: images no object points at, older than 30 days. Not on each
  /// delete, so an undone delete finds its image.
  private func dropUnusedAssets() {
    let used = objectsByID.values.reduce(into: Set<String>()) { $0.formUnion(Easels.assets(of: $1)) }
    let cutoff = Easels.syncTime(now().addingTimeInterval(-EaselLimits.unusedAssetLifetime))
    let gone = assetsBySHA.values.filter { !used.contains($0.sha256) && $0.createTime < cutoff }.map(\.sha256)
    guard !gone.isEmpty else { return }
    forget(assets: gone)
    scheduleSave()
  }

  private func load(_ directory: URL) {
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    guard let data = try? Data(contentsOf: directory.appendingPathComponent(Self.fileName)) else { return }
    for specifics in Self.records(data) {
      switch specifics.entity {
      case .easel(let e) where Easels.isLowerUUID(e.uuid): easelsByID[e.uuid] = e
      case .easelObject(let o) where Easels.isLowerUUID(o.uuid): objectsByID[o.uuid] = o
      case .easelAsset(let a) where Easels.isSha256Hex(a.sha256):
        // Its bytes are checked against the hash when read.
        guard let url = assetURL(a.sha256),
          let size = (try? FileManager.default.attributesOfItem(atPath: url.path))?[.size] as? Int
        else { continue }
        var meta = a
        meta.clearData()
        assetsBySHA[a.sha256] = meta
        assetSizes[a.sha256] = size
      default: break
      }
    }
  }

  /// easels.pb's records: a varint length, then a CompassSpacesSpecifics.
  /// A record cut short ends the file; one that doesn't parse is skipped.
  static func records(_ data: Data) -> [SyncPb_CompassSpacesSpecifics] {
    let bytes = [UInt8](data)
    var pos = 0
    var out: [SyncPb_CompassSpacesSpecifics] = []
    while pos < bytes.count {
      var size: UInt64 = 0
      var shift: UInt64 = 0
      var ended = false
      while pos < bytes.count && shift < 64 {
        let byte = bytes[pos]
        pos += 1
        size |= UInt64(byte & 0x7f) << shift
        if byte & 0x80 == 0 {
          ended = true
          break
        }
        shift += 7
      }
      guard ended, size <= UInt64(bytes.count - pos) else { break }
      let end = pos + Int(size)
      if let specifics = try? SyncPb_CompassSpacesSpecifics(serializedBytes: Array(bytes[pos..<end])) {
        out.append(specifics)
      }
      pos = end
    }
    return out
  }

  static func appendRecord(_ specifics: SyncPb_CompassSpacesSpecifics, to out: inout Data) {
    guard let bytes: Data = try? specifics.serializedBytes() else { return }
    var value = UInt64(bytes.count)
    while value >= 0x80 {
      out.append(UInt8(value & 0x7f) | 0x80)
      value >>= 7
    }
    out.append(UInt8(value))
    out.append(bytes)
  }

  /// The file's contents now.
  func serialized() -> Data {
    var out = Data()
    for key in easelsByID.keys.sorted() {
      var s = SyncPb_CompassSpacesSpecifics()
      s.easel = easelsByID[key]!
      Self.appendRecord(s, to: &out)
    }
    for key in objectsByID.keys.sorted() {
      var s = SyncPb_CompassSpacesSpecifics()
      s.easelObject = objectsByID[key]!
      Self.appendRecord(s, to: &out)
    }
    for key in assetsBySHA.keys.sorted() {
      var s = SyncPb_CompassSpacesSpecifics()
      s.easelAsset = assetsBySHA[key]!
      Self.appendRecord(s, to: &out)
    }
    return out
  }

  /// Writes pending changes now (backgrounding, tests).
  public func flush() {
    saveTask?.cancel()
    saveTask = nil
    guard let directory else { return }
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    #if os(iOS)
      let options: Data.WritingOptions = [.atomic, .completeFileProtectionUntilFirstUserAuthentication]
    #else
      let options: Data.WritingOptions = [.atomic]
    #endif
    try? serialized().write(to: directory.appendingPathComponent(Self.fileName), options: options)
  }

  /// A second after the last change, as the desktop's writer: a drag of
  /// many objects writes once.
  private func scheduleSave() {
    guard directory != nil, saveTask == nil else { return }
    saveTask = Task { [weak self] in
      try? await Task.sleep(for: .seconds(1))
      guard !Task.isCancelled, let self else { return }
      self.saveTask = nil
      self.flush()
    }
  }

  private func changed() {
    scheduleSave()
    onChange?()
    for observer in changeObservers { observer() }
  }
}
