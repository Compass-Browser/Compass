// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CryptoKit
import Foundation
import SwiftProtobuf
import SyncProto

public enum SyncConnectionError: Error, Equatable {
  /// No reply (offline, DNS, TLS…).
  case network(String)
  /// An HTTP status other than 200.
  case http(Int)
  /// 401 from the server's sign-in gate (`X-Compass-Login: required`): the
  /// chain must sign in (LoginClient) before it syncs.
  case loginRequired
  case badReply
  /// Another device removed this one from the chain: the server refuses
  /// its cache GUID (403 with `X-Compass-Device: removed`) until it leaves
  /// and joins again with a new one.
  case deviceRemoved

  /// What a reply other than `expected` means.
  static func status(_ response: HTTPURLResponse) -> SyncConnectionError {
    if response.statusCode == 403,
      response.value(forHTTPHeaderField: "X-Compass-Device")?.lowercased() == "removed"
    {
      return .deviceRemoved
    }
    return .http(response.statusCode)
  }
}

/// The server's endpoints: `<server>/<path>?<query>`, the server URL ending
/// in /v2. Values are escaped as Chromium's EscapeQueryParamValue() does:
/// a cache GUID is base64, and a bare `+` would read as a space on the
/// server, so it wouldn't match the GUID in DEVICE_INFO.
enum ServerEndpoint {
  /// RFC 3986's unreserved characters, the only ones left as they are.
  static let unreserved = CharacterSet(
    charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")

  static func escape(_ value: String) -> String {
    value.addingPercentEncoding(withAllowedCharacters: unreserved) ?? value
  }

  /// `path` is escaped already (see `escape`) and has no leading slash.
  static func url(_ server: URL, path: String, query: KeyValuePairs<String, String>) -> URL {
    var c = URLComponents(url: server, resolvingAgainstBaseURL: false)!
    var base = c.percentEncodedPath
    while base.hasSuffix("/") { base.removeLast() }
    c.percentEncodedPath = base + "/" + path
    c.percentEncodedQuery =
      query.isEmpty ? nil : query.map { escape($0.key) + "=" + escape($0.value) }.joined(separator: "&")
    return c.url!
  }
}

/// Posts ClientToServerMessages to the server's /command/ endpoint.
public protocol SyncConnection: Sendable {
  func post(_ message: SyncPb_ClientToServerMessage) async throws -> SyncPb_ClientToServerResponse
}

/// The sync server over HTTPS, signed with the chain's key, as the desktop's
/// CompassConnectionManager: POST <server>/command/?client=…&client_id=<cache guid>.
public struct HTTPSyncConnection: SyncConnection {
  public static let clientName = "Compass iOS"
  let url: URL
  let authKey: Data
  let session: URLSession

  public init(serverURL: URL, cacheGUID: String, authKey: Data, session: URLSession = .shared) {
    url = Self.commandURL(serverURL, cacheGUID: cacheGUID)
    self.authKey = authKey
    self.session = session
  }

  static func commandURL(_ server: URL, cacheGUID: String) -> URL {
    ServerEndpoint.url(server, path: "command/", query: ["client": clientName, "client_id": cacheGUID])
  }

  public func post(_ message: SyncPb_ClientToServerMessage) async throws -> SyncPb_ClientToServerResponse {
    var request = URLRequest(url: url, timeoutInterval: 60)
    request.httpMethod = "POST"
    request.httpBody = try message.serializedBytes()
    request.setValue("application/octet-stream", forHTTPHeaderField: "Content-Type")
    request.setValue(
      "Bearer " + Chain.accessToken(authKey: authKey, now: Date()), forHTTPHeaderField: "Authorization")
    let data: Data
    let response: URLResponse
    do {
      (data, response) = try await session.data(for: request)
    } catch {
      throw SyncConnectionError.network(error.localizedDescription)
    }
    guard let http = response as? HTTPURLResponse else { throw SyncConnectionError.badReply }
    if LoginGate.isLoginRequired(http) { throw SyncConnectionError.loginRequired }
    guard http.statusCode == 200 else { throw SyncConnectionError.status(http) }
    do {
      return try SyncPb_ClientToServerResponse(serializedBytes: data)
    } catch {
      throw SyncConnectionError.badReply
    }
  }
}

/// Chromium's ClientTagHash::FromUnhashed: base64(SHA1(serialized empty
/// specifics of the type || client tag)). The server keys entities on it,
/// so it must match the desktop's byte for byte.
public enum ClientTagHash {
  public static func make(type: DataType, tag: String) -> String {
    make(emptySpecifics: type.emptySpecifics, tag: tag)
  }

  static func make(emptySpecifics: SyncPb_EntitySpecifics, tag: String) -> String {
    var input = (try? emptySpecifics.serializedBytes() as [UInt8]) ?? []
    input.append(contentsOf: Array(tag.utf8))
    return Data(Insecure.SHA1.hash(data: input)).base64EncodedString()
  }
}
