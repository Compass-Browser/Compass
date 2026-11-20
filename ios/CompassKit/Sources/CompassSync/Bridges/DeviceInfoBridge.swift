// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import SyncProto

/// A device in the chain, from DEVICE_INFO.
public struct SyncDevice: Sendable, Equatable, Identifiable {
  public var id: String { cacheGUID }
  public var cacheGUID: String
  public var name: String
  public var formFactor: SyncPb_SyncEnums.DeviceFormFactor
  public var osType: SyncPb_SyncEnums.OsType
  public var lastUpdated: Date
  public var isThisDevice: Bool
}

/// DEVICE_INFO: this phone's entry (so the Mac's device list shows it) and
/// everyone else's. Tag = "DeviceInfo_" + cache GUID, as Chromium's
/// DeviceInfoUtil. The entry is refreshed once a day (Chromium's pulse), so
/// other devices don't consider the phone gone.
public final class DeviceInfoBridge: DataTypeBridge, @unchecked Sendable {
  public static let pulseInterval: TimeInterval = 24 * 60 * 60
  public let dataType = DataType.deviceInfo

  public struct LocalDevice: Sendable {
    public var name: String
    public var model: String
    public var appVersion: String
    public var formFactor: SyncPb_SyncEnums.DeviceFormFactor = .phone
    public init(name: String, model: String, appVersion: String) {
      self.name = name
      self.model = model
      self.appVersion = appVersion
    }
  }

  private let lock = NSLock()
  private var devices: [String: SyncPb_DeviceInfoSpecifics] = [:]
  private let local: LocalDevice
  private var cacheGUID = ""
  /// Called with the full list after every change.
  public var onChange: (@Sendable ([SyncDevice]) -> Void)?
  /// Where the entries are kept between launches: the engine downloads
  /// only what changed since its progress marker, so without them a
  /// relaunched app knew only the devices that pulsed since.
  let stateURL: URL?
  /// Whether the entries weren't kept (a first run, or an app from before
  /// they were): the engine should download them again (`refetch`).
  public let needsRefetch: Bool

  /// `cacheGUID`: this device's, when known before the first cycle (so
  /// its saved entry shows as this device's at once).
  public init(local: LocalDevice, cacheGUID: String = "", stateURL: URL? = nil) {
    self.local = local
    self.cacheGUID = cacheGUID
    self.stateURL = stateURL
    if let stateURL, let data = try? Data(contentsOf: stateURL),
      let saved = try? PropertyListDecoder().decode([String: Data].self, from: data)
    {
      devices = saved.compactMapValues { try? SyncPb_DeviceInfoSpecifics(serializedBytes: $0) }
      needsRefetch = false
    } else {
      needsRefetch = stateURL != nil
    }
  }

  private func save() {
    guard let stateURL else { return }
    let saved = lock.withLock { devices.compactMapValues { try? $0.serializedBytes() as Data } }
    guard let data = try? PropertyListEncoder().encode(saved) else { return }
    try? data.write(to: stateURL, options: .atomic)
  }

  public static func tag(cacheGUID: String) -> String { "DeviceInfo_" + cacheGUID }

  public func clientTag(for specifics: SyncPb_EntitySpecifics) -> String {
    Self.tag(cacheGUID: specifics.deviceInfo.cacheGuid)
  }

  public var allDevices: [SyncDevice] {
    lock.withLock {
      devices.values.map {
        SyncDevice(
          cacheGUID: $0.cacheGuid, name: $0.clientName, formFactor: $0.deviceFormFactor,
          osType: $0.osType,
          lastUpdated: Date(timeIntervalSince1970: TimeInterval($0.lastUpdatedTimestamp) / 1000),
          isThisDevice: $0.cacheGuid == cacheGUID)
      }.sorted { $0.lastUpdated > $1.lastUpdated }
    }
  }

  /// This device's name, as other devices show it.
  public var localName: String { local.name }

  /// The devices a tab can be sent to, newest first, as Chromium's
  /// SendTabToSelfBridge::GetTargetDeviceInfoSortedList(): those that take
  /// sent tabs, seen in the last 10 days, not this one.
  public func sendTargets(now: Date = Date()) -> [SyncDevice] {
    let receiving = lock.withLock {
      Set(devices.values.filter(\.featureFields.sendTabToSelfReceivingEnabled).map(\.cacheGuid))
    }
    return allDevices.filter {
      !$0.isThisDevice && receiving.contains($0.cacheGUID)
        && now.timeIntervalSince($0.lastUpdated) <= Self.sendTargetExpiry
    }
  }

  /// Chromium's kDeviceExpiration.
  public static let sendTargetExpiry: TimeInterval = 10 * 24 * 60 * 60

  public func apply(_ changes: [RemoteChange]) async {
    lock.withLock {
      for change in changes {
        if let specifics = change.specifics {
          devices[specifics.deviceInfo.cacheGuid] = specifics.deviceInfo
        } else {
          devices = devices.filter {
            ClientTagHash.make(type: .deviceInfo, tag: Self.tag(cacheGUID: $0.key)) != change.clientTagHash
          }
        }
      }
    }
    save()
    onChange?(allDevices)
  }

  /// Drops a device removed from here (DeviceRemover) before its tombstone
  /// comes down. Never this device's own entry.
  public func forget(_ cacheGUID: String) {
    let changed = lock.withLock {
      guard cacheGUID != self.cacheGUID else { return false }
      return devices.removeValue(forKey: cacheGUID) != nil
    }
    if changed {
      save()
      onChange?(allDevices)
    }
  }

  /// This device's specifics, as Chromium's LocalDeviceInfoProvider fills them.
  public func localSpecifics(cacheGUID: String, now: Date = Date()) -> SyncPb_EntitySpecifics {
    var info = SyncPb_DeviceInfoSpecifics()
    info.cacheGuid = cacheGUID
    info.clientName = local.name
    info.deviceType = .typePhone
    info.syncUserAgent = "Compass iOS \(local.appVersion)"
    info.chromeVersion = local.appVersion
    info.signinScopedDeviceID = cacheGUID
    info.lastUpdatedTimestamp = Int64(now.timeIntervalSince1970 * 1000)
    info.model = local.model
    info.manufacturer = "Apple"
    info.osType = .ios
    info.deviceFormFactor = local.formFactor
    info.pulseIntervalInMinutes = Int32(Self.pulseInterval / 60)
    // A target of the desktop's "Send to your devices" (SendTabBridge).
    info.featureFields.sendTabToSelfReceivingEnabled = true
    info.featureFields.sendTabToSelfReceivingType = .chromeAndPushNotification
    var specifics = SyncPb_EntitySpecifics()
    specifics.deviceInfo = info
    return specifics
  }

  /// Puts this device's entry when it's missing, renamed or older than a day.
  public func refreshLocal(engine: SyncEngine, now: Date = Date()) async {
    let guid = await engine.cacheGUID
    let existing = lock.withLock {
      cacheGUID = guid
      return devices[guid]
    }
    let stale = existing.map {
      now.timeIntervalSince1970 * 1000 - Double($0.lastUpdatedTimestamp) > Self.pulseInterval * 1000
        || $0.clientName != local.name
        // Entries from before the phone received sent tabs.
        || !$0.featureFields.sendTabToSelfReceivingEnabled
    } ?? true
    guard stale else { return }
    let specifics = localSpecifics(cacheGUID: guid, now: now)
    lock.withLock { devices[guid] = specifics.deviceInfo }
    save()
    await engine.put(.deviceInfo, tag: Self.tag(cacheGUID: guid), specifics: specifics)
  }
}
