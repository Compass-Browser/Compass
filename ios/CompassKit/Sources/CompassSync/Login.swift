// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation

/// Company sign-in (docs/design/sync.md, "Company sign-in (OIDC)"): a server
/// can require that a chain be bound to a user of the company's identity
/// provider before it syncs. The server does the OAuth work; the phone
/// creates a login request with its chain token, opens the returned URL in
/// a browser (ASWebAuthenticationSession) and long-polls for the result.
/// Login gates access only: keys and encryption stay as they are.
///
/// Paths are under the server URL, which ends in /v2 as for /command/.
public struct LoginClient: Sendable {
  /// The signed-in user, as the server shows it.
  public struct User: Equatable, Sendable, Decodable {
    public var name: String
    public var email: String
    public init(name: String, email: String) {
      self.name = name
      self.email = email
    }
  }

  /// The chain's binding (`GET /compass/account`).
  public struct Account: Equatable, Sendable {
    /// The server requires sign-in and this chain isn't (or no longer)
    /// bound.
    public var loginRequired: Bool
    public var user: User?
    /// When the sign-in ends; nil when it doesn't (a refresh token keeps it).
    public var expires: Date?
    public init(loginRequired: Bool, user: User?, expires: Date?) {
      self.loginRequired = loginRequired
      self.user = user
      self.expires = expires
    }
  }

  /// A pending login (`POST /compass/login`).
  public struct Request: Equatable, Sendable {
    public var id: String
    /// Where the browser goes: the server's start page, which redirects to
    /// the identity provider.
    public var url: URL
    /// "K7F-2QM": the server's consent page shows the same code.
    public var userCode: String
    public var expires: Date
  }

  public enum Status: Equatable, Sendable {
    case pending
    case done(User?)
    case denied
    /// Expired, or unknown to the server (a restart drops pending logins).
    case expired
  }

  public enum LoginError: Error, Equatable {
    /// The server has no sign-in (404 login_disabled).
    case disabled
    /// The identity provider isn't reached yet, or too many logins (503).
    case unavailable
    /// 429: too many login requests.
    case rateLimited
    /// The chain token was refused.
    case unauthorized
    case http(Int)
    case network(String)
    case badReply
  }

  /// The callback of `return: "compass"`: the server's approve/deny ends in
  /// a redirect to compass://sync-login-done?status=done|denied, which the
  /// ASWebAuthenticationSession captures.
  public static let callbackScheme = "compass"
  public static let callbackHost = "sync-login-done"

  /// Whether `url` is that callback (not a page or a DeepLink).
  public static func isCallback(_ url: URL) -> Bool {
    url.scheme?.lowercased() == callbackScheme && url.host()?.lowercased() == callbackHost
  }

  /// "done" or "denied" from a callback URL.
  public static func callbackStatus(_ url: URL) -> String? {
    guard isCallback(url) else { return nil }
    return URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first { $0.name == "status" }?.value
  }

  let server: URL
  let authKey: Data
  let session: URLSession

  public init(serverURL: URL, authKey: Data, session: URLSession = .shared) {
    server = serverURL
    self.authKey = authKey
    self.session = session
  }

  static func url(_ server: URL, _ path: String, query: [URLQueryItem] = []) -> URL {
    var c = URLComponents(url: server, resolvingAgainstBaseURL: false)!
    var base = c.path
    while base.hasSuffix("/") { base.removeLast() }
    c.path = base + "/compass/" + path
    c.queryItems = query.isEmpty ? nil : query
    return c.url!
  }

  // MARK: Requests

  /// The server's sign-in, if it has one: the provider's name ("Acme SSO").
  /// No auth, so a phone can ask before it joins or starts a chain.
  public static func info(serverURL: URL, session: URLSession = .shared) async throws -> String? {
    let request = URLRequest(url: url(serverURL, "info"), timeoutInterval: 20)
    let data = try await send(request, session: session, expect: 200)
    guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
      throw LoginError.badReply
    }
    guard let login = json["login"] as? [String: Any] else { return nil }
    return (login["name"] as? String) ?? ""
  }

  /// The chain's binding. Never gated: it tells "sign in" apart from a
  /// refused key.
  public func account() async throws -> Account {
    let data = try await send(signed("account", method: "GET"), session: session, expect: 200)
    struct Reply: Decodable {
      var login_required: Bool?
      var user: User?
      var expires: Int64?
    }
    guard let reply = try? JSONDecoder().decode(Reply.self, from: data) else { throw LoginError.badReply }
    let expires = reply.expires.flatMap { $0 > 0 ? Date(timeIntervalSince1970: Double($0) / 1000) : nil }
    return Account(loginRequired: reply.login_required ?? false, user: reply.user, expires: expires)
  }

  /// Starts a login; the server shows `deviceName` on its consent page.
  public func start(deviceName: String) async throws -> Request {
    var request = signed("login", method: "POST")
    request.setValue("application/json", forHTTPHeaderField: "Content-Type")
    request.httpBody = try JSONSerialization.data(withJSONObject: ["device_name": deviceName, "return": "compass"])
    let data = try await send(request, session: session, expect: 200)
    guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
      let id = json["id"] as? String, !id.isEmpty,
      let link = (json["url"] as? String).flatMap(URL.init(string:)),
      let code = json["user_code"] as? String,
      let expires = (json["expires"] as? NSNumber)?.int64Value
    else { throw LoginError.badReply }
    return Request(
      id: id, url: link, userCode: code, expires: Date(timeIntervalSince1970: Double(expires) / 1000))
  }

  /// The login's state; with `wait`, the server holds the request up to
  /// 30 s while it's pending.
  public func poll(_ id: String, wait: Bool = true) async throws -> Status {
    let escaped = id.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed.subtracting(["/"])) ?? id
    var request = signed("login/" + escaped, method: "GET", query: wait ? [] : [URLQueryItem(name: "wait", value: "0")])
    request.timeoutInterval = 45
    let data: Data
    do {
      data = try await send(request, session: session, expect: 200)
    } catch LoginError.http(404) {
      return .expired
    }
    struct Reply: Decodable {
      var status: String
      var user: User?
    }
    guard let reply = try? JSONDecoder().decode(Reply.self, from: data) else { throw LoginError.badReply }
    switch reply.status {
    case "pending": return .pending
    case "done": return .done(reply.user)
    case "denied": return .denied
    case "expired": return .expired
    default: throw LoginError.badReply
    }
  }

  /// Unbinds the chain (Leave). 204.
  public func signOut() async throws {
    _ = try await send(signed("account", method: "DELETE"), session: session, expect: 204)
  }

  /// Gives a profile's own account (another chain) this chain's user, so it
  /// syncs without a second sign-in. This chain must be signed in.
  public func link(profileAuthKey: Data) async throws {
    var request = signed("account/link", method: "POST")
    request.setValue(
      Chain.accessToken(authKey: profileAuthKey, now: Date()), forHTTPHeaderField: "X-Compass-Linked-Token")
    _ = try await send(request, session: session, expect: 204)
  }

  private func signed(_ path: String, method: String, query: [URLQueryItem] = []) -> URLRequest {
    var request = URLRequest(url: Self.url(server, path, query: query), timeoutInterval: 30)
    request.httpMethod = method
    request.setValue("Bearer " + Chain.accessToken(authKey: authKey, now: Date()), forHTTPHeaderField: "Authorization")
    return request
  }

  private static func send(_ request: URLRequest, session: URLSession, expect: Int) async throws -> Data {
    let data: Data
    let response: URLResponse
    do {
      (data, response) = try await session.data(for: request)
    } catch {
      throw LoginError.network(error.localizedDescription)
    }
    guard let http = response as? HTTPURLResponse else { throw LoginError.badReply }
    if http.statusCode == expect { return data }
    let error = (try? JSONSerialization.jsonObject(with: data) as? [String: Any])?["error"] as? String
    switch (http.statusCode, error) {
    case (404, "login_disabled"): throw LoginError.disabled
    case (503, _): throw LoginError.unavailable
    case (429, _): throw LoginError.rateLimited
    case (401, _): throw LoginError.unauthorized
    default: throw LoginError.http(http.statusCode)
    }
  }

  private func send(_ request: URLRequest, session: URLSession, expect: Int) async throws -> Data {
    try await Self.send(request, session: session, expect: expect)
  }
}

/// Whether a 401 is the sign-in gate's (`X-Compass-Login: required`)
/// rather than a refused key.
public enum LoginGate {
  public static let header = "X-Compass-Login"

  public static func isLoginRequired(_ response: HTTPURLResponse) -> Bool {
    response.statusCode == 401 && response.value(forHTTPHeaderField: header)?.lowercased() == "required"
  }
}
