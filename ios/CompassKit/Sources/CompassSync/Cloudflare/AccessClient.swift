// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// A friend on the server's access list.
public struct ServerMember: Sendable, Equatable, Identifiable {
  /// Their chain ID (lower-case hex).
  public var chain: String
  /// The name the owner gave the invite they joined with (may be empty).
  public var name: String
  public var added: Date
  /// Their profile accounts linked to it.
  public var linked: Int
  public var id: String { chain }
}

/// An invite not used yet.
public struct ServerInvite: Sendable, Equatable, Identifiable {
  public var code: String
  public var name: String
  public var expires: Date
  public var id: String { code }
}

public struct ServerPeople: Sendable, Equatable {
  public var members: [ServerMember] = []
  public var invites: [ServerInvite] = []
  /// Whether the server wakes phones through the Compass push relay
  /// (docs/design/sync.md, "Push on a Cloudflare server"); nil for a server
  /// without that setting (before version 3).
  public var pushRelay: Bool?
  /// The relay it uses; empty when the server has push turned off for good
  /// (`COMPASS_SYNC_PUSH_RELAY=off`), so the owners can't turn it on.
  public var pushRelayURL = ""

  public init() {}

  /// The relay's host, for Settings' words ("" when there's none).
  public var pushRelayHost: String { URL(string: pushRelayURL)?.host() ?? "" }
}

/// An invite link, `https://<server>/invite#<code>` (server/cloudflare,
/// "Who may use it"): the sync server URL (`<server>/v2`) and the code.
public struct ServerInviteLink: Sendable, Equatable {
  public var serverURL: URL
  public var code: String

  /// nil unless `text` (trimmed) is such a link.
  public init?(_ text: String) {
    guard var c = URLComponents(string: text.trimmingCharacters(in: .whitespacesAndNewlines)),
      let scheme = c.scheme?.lowercased(), scheme == "https" || scheme == "http",
      c.host?.isEmpty == false, let fragment = c.fragment, c.path.hasSuffix("/invite")
    else { return nil }
    let code = fragment.lowercased()
    guard (10...64).contains(code.count),
      code.allSatisfy({ ("a"..."z").contains($0) || ("2"..."7").contains($0) })
    else { return nil }
    c.path = String(c.path.dropLast("/invite".count)) + "/v2"
    c.query = nil
    c.fragment = nil
    guard let url = c.url else { return nil }
    serverURL = url
    self.code = code
  }
}

/// The access list of a server that has one (the Cloudflare server,
/// server/cloudflare/README.md, "Who may use it"): its owners invite friends
/// with single-use codes, and friends join with them (docs/design/sync.md,
/// "Inviting friends"). A port of the desktop's
/// components/sync/access_client.cc. Every request but `info` is signed with
/// a chain token (`Chain.accessToken`); paths are under the server URL,
/// which ends in /v2.
public struct AccessClient: Sendable {
  public enum Failure: Error, Sendable, Equatable {
    /// 404 or 405: a server without an access list (the Go server).
    case unsupported
    /// 403: only the server's owners may do that.
    case notOwner
    /// 401: the server refused the chain.
    case unauthorized
    /// Join: no such invite.
    case noSuchInvite
    /// Join: the invite was used or has expired (410).
    case inviteGone
    /// 429: too many invites or friends.
    case tooMany
    /// Anything else, or no answer.
    case failed

    /// The desktop's AccessClient::Describe().
    public var message: String {
      switch self {
      case .unsupported: String(localized: "This server doesn't take invites.", bundle: .module, comment: "Sync server invites: error.")
      case .notOwner: String(localized: "Only the server's owner can do that.", bundle: .module, comment: "Sync server invites: error.")
      case .unauthorized: String(localized: "The server refused this device.", bundle: .module, comment: "Sync server invites: error.")
      case .noSuchInvite: String(localized: "This invite doesn't exist. Check the link.", bundle: .module, comment: "Sync server invites: error.")
      case .inviteGone: String(localized: "This invite was used or has expired. Ask for a new one.", bundle: .module, comment: "Sync server invites: error.")
      case .tooMany: String(localized: "The server has too many invites or people. Remove some first.", bundle: .module, comment: "Sync server invites: error.")
      case .failed: String(localized: "Couldn't reach the sync server. Try again.", bundle: .module, comment: "Sync server invites: error.")
      }
    }

    static func of(_ status: Int, _ json: [String: Any]?) -> Failure? {
      switch status {
      case 200..<300: nil
      case 401: .unauthorized
      case 403: .notOwner
      case 404: json?["error"] as? String == "no_such_invite" ? .noSuchInvite : .unsupported
      case 405: .unsupported
      case 410: .inviteGone
      case 429: .tooMany
      default: .failed
      }
    }
  }

  public let serverURL: URL
  let session: URLSession

  public init(serverURL: URL, session: URLSession = .shared) {
    self.serverURL = serverURL
    self.session = session
  }

  /// `GET /compass/info`, unsigned: what the server says of itself (nil:
  /// no answer, or not JSON).
  public func info() async -> ServerInfo? {
    guard let (status, data) = try? await send("GET", "compass/info", token: ""), status == 200 else { return nil }
    return ServerInfo.parse(data)
  }

  /// `GET /compass/access` (owners).
  public func list(token: String) async throws(Failure) -> ServerPeople {
    let (status, json) = try await sendJSON("GET", "compass/access", token: token)
    if let failure = Failure.of(status, json) { throw failure }
    guard let json else { throw .failed }
    var people = ServerPeople()
    for case let m as [String: Any] in json["members"] as? [Any] ?? [] {
      guard let chain = m["chain"] as? String else { continue }
      people.members.append(
        ServerMember(
          chain: chain, name: m["name"] as? String ?? "",
          added: Date(timeIntervalSince1970: (m["added_at"] as? NSNumber)?.doubleValue ?? 0),
          linked: (m["linked"] as? NSNumber)?.intValue ?? 0))
    }
    let now = Date()
    for case let i as [String: Any] in json["invites"] as? [Any] ?? [] {
      guard let code = i["code"] as? String else { continue }
      people.invites.append(
        ServerInvite(
          code: code, name: i["name"] as? String ?? "",
          expires: now.addingTimeInterval((i["expires_in"] as? NSNumber)?.doubleValue ?? 0)))
    }
    if let settings = json["settings"] as? [String: Any] {
      people.pushRelay = settings["push_relay"] as? Bool
      people.pushRelayURL = settings["push_relay_url"] as? String ?? ""
    }
    return people
  }

  /// `POST /compass/access/invites {"name"}` (owners): the invite's link.
  public func createInvite(token: String, name: String) async throws(Failure) -> String {
    let (status, json) = try await sendJSON("POST", "compass/access/invites", token: token, body: ["name": name])
    if let failure = Failure.of(status, json) { throw failure }
    guard let link = json?["link"] as? String else { throw .failed }
    return link
  }

  /// `DELETE /compass/access/{chain or code}` (owners): removes a friend
  /// (and their linked profiles) or revokes an invite.
  public func remove(token: String, target: String) async throws(Failure) {
    let (status, json) = try await sendJSON("DELETE", "compass/access/" + ServerEndpoint.escape(target), token: token)
    if let failure = Failure.of(status, json) { throw failure }
  }

  /// `PUT /compass/access/settings {"push_relay"}` (owners).
  public func setPushRelay(token: String, on: Bool) async throws(Failure) {
    let (status, json) = try await sendJSON("PUT", "compass/access/settings", token: token, body: ["push_relay": on])
    if let failure = Failure.of(status, json) { throw failure }
  }

  /// `POST /compass/access/join {"code"}`, with the joining chain's token.
  public func join(token: String, code: String) async throws(Failure) {
    let (status, json) = try await sendJSON("POST", "compass/access/join", token: token, body: ["code": code])
    if let failure = Failure.of(status, json) { throw failure }
  }

  private func sendJSON(_ method: String, _ path: String, token: String, body: [String: Any]? = nil)
    async throws(Failure) -> (Int, [String: Any]?)
  {
    let (status, data) = try await send(method, path, token: token, body: body)
    return (status, try? JSONSerialization.jsonObject(with: data) as? [String: Any])
  }

  private func send(_ method: String, _ path: String, token: String, body: [String: Any]? = nil)
    async throws(Failure) -> (Int, Data)
  {
    var request = URLRequest(url: ServerEndpoint.url(serverURL, path: path, query: [:]), timeoutInterval: 30)
    request.httpMethod = method
    request.httpShouldHandleCookies = false
    if !token.isEmpty { request.setValue("Bearer " + token, forHTTPHeaderField: "Authorization") }
    if let body {
      request.setValue("application/json", forHTTPHeaderField: "Content-Type")
      request.httpBody = try? JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])
    }
    do {
      let (data, response) = try await session.data(for: request)
      guard let http = response as? HTTPURLResponse else { throw Failure.failed }
      return (http.statusCode, data)
    } catch {
      throw .failed
    }
  }
}
