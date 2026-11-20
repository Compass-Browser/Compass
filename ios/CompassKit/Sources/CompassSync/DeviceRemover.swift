// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation

/// Removes another device from the chain's account (docs/design/sync.md,
/// "Removing a device"): `DELETE <server>/compass/devices/<its cache
/// GUID>?client_id=<this device's>`, signed with the account's token. The
/// server deletes its DEVICE_INFO entry (the other devices get the
/// tombstone with their next sync) and refuses that cache GUID from then
/// on: the removed device gets 403 `X-Compass-Device: removed` everywhere
/// until it leaves and joins again.
public struct DeviceRemover: Sendable {
  public enum Failure: Error, Equatable {
    /// 409: the server won't let a device remove itself (that's Leave).
    case thisDevice
    /// 403 device_removed: this device was removed itself.
    case deviceRemoved
    /// 401 with `X-Compass-Login`: the server wants a sign-in first.
    case loginRequired
    case http(Int)
    case network(String)
    case badReply

    /// What Settings says.
    public var message: String {
      switch self {
      case .thisDevice: "To stop syncing this device, use Leave Sync."
      case .deviceRemoved: "This device was removed from sync on another device."
      case .loginRequired: "The sync server asks you to sign in. Sign in on a computer, then try again."
      case .http(401): "The sync server didn't accept this device's key."
      case .http(let code): "The sync server refused (HTTP \(code))."
      case .network(let message): "Can't reach the sync server (\(message))."
      case .badReply: "The sync server's reply wasn't understood."
      }
    }
  }

  let server: URL
  let cacheGUID: String
  let authKey: Data
  let session: URLSession

  /// `cacheGUID` and `authKey` are this device's, in the account the
  /// device to remove is listed in.
  public init(serverURL: URL, cacheGUID: String, authKey: Data, session: URLSession = .shared) {
    server = serverURL
    self.cacheGUID = cacheGUID
    self.authKey = authKey
    self.session = session
  }

  /// The GUID is escaped as one path segment: base64 has `/` and `+`.
  static func url(_ server: URL, device: String, cacheGUID: String) -> URL {
    ServerEndpoint.url(
      server, path: "compass/devices/" + ServerEndpoint.escape(device), query: ["client_id": cacheGUID])
  }

  /// Removes the device with this cache GUID; done (204) also when it was
  /// gone already.
  public func remove(_ device: String) async throws {
    var request = URLRequest(url: Self.url(server, device: device, cacheGUID: cacheGUID), timeoutInterval: 30)
    request.httpMethod = "DELETE"
    request.setValue("Bearer " + Chain.accessToken(authKey: authKey, now: Date()), forHTTPHeaderField: "Authorization")
    let data: Data
    let response: URLResponse
    do {
      (data, response) = try await session.data(for: request)
    } catch {
      throw Failure.network(error.localizedDescription)
    }
    guard let http = response as? HTTPURLResponse else { throw Failure.badReply }
    switch http.statusCode {
    case 200, 204: return
    case 409: throw Failure.thisDevice
    case 401 where http.value(forHTTPHeaderField: "X-Compass-Login") != nil || Self.error(data) == "login_required":
      throw Failure.loginRequired
    case 403 where SyncConnectionError.status(http) == .deviceRemoved || Self.error(data) == "device_removed":
      throw Failure.deviceRemoved
    default: throw Failure.http(http.statusCode)
    }
  }

  /// The `error` of a JSON reply.
  static func error(_ data: Data) -> String? {
    (try? JSONSerialization.jsonObject(with: data) as? [String: Any])?["error"] as? String
  }
}
