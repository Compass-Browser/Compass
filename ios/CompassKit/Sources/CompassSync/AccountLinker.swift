// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation

/// Links a profile's account to the chain's own on the server
/// (docs/design/sync.md, "Profiles"): `POST <server>/compass/account/link`
/// with the home account's token, and the profile account's in
/// `X-Compass-Linked-Token`. A private server (an allowlist of chains) lets
/// linked accounts in with the chain that linked them; a server with
/// sign-in binds them to the same user. Other servers answer 204 and keep
/// the link, or 404 (older ones): either way there's nothing more to do.
public struct AccountLinker: Sendable {
  public enum Result: Equatable, Sendable {
    /// 204, or a server without the endpoint (404, 405): nothing more to do.
    case linked
    /// 401: the server refused the home account itself (not listed, or
    /// not signed in).
    case refused
    /// Anything else: try again later.
    case failed(String)
  }

  public static let linkedTokenHeader = "X-Compass-Linked-Token"

  let server: URL
  let session: URLSession

  public init(serverURL: URL, session: URLSession = .shared) {
    server = serverURL
    self.session = session
  }

  static func url(_ server: URL) -> URL { ServerEndpoint.url(server, path: "compass/account/link", query: [:]) }

  /// Links the account of `profileUUID` (not empty) from the home account.
  public func link(chain: Chain, profileUUID: String) async -> Result {
    var request = URLRequest(url: Self.url(server), timeoutInterval: 30)
    request.httpMethod = "POST"
    request.setValue("Bearer " + chain.accessToken(), forHTTPHeaderField: "Authorization")
    request.setValue(chain.accessToken(profileUUID: profileUUID), forHTTPHeaderField: Self.linkedTokenHeader)
    do {
      let (_, response) = try await session.data(for: request)
      guard let http = response as? HTTPURLResponse else { return .failed("no reply") }
      return Self.result(status: http.statusCode)
    } catch {
      return .failed(error.localizedDescription)
    }
  }

  static func result(status: Int) -> Result {
    switch status {
    case 200..<300, 404, 405: .linked
    case 401: .refused
    default: .failed("HTTP \(status)")
    }
  }
}
