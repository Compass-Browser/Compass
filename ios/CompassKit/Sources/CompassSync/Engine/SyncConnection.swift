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
  case badReply
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
    var c = URLComponents(url: server, resolvingAgainstBaseURL: false)!
    var path = c.path
    if path.hasSuffix("/") { path.removeLast() }
    c.path = path + "/command/"
    c.queryItems = [
      URLQueryItem(name: "client", value: clientName), URLQueryItem(name: "client_id", value: cacheGUID),
    ]
    return c.url!
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
    guard http.statusCode == 200 else { throw SyncConnectionError.http(http.statusCode) }
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
