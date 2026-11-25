// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CryptoKit
import Foundation

/// "Sign in with Cloudflare" for setting up or updating a server
/// (docs/design/sync.md, "A free server on Cloudflare"; the desktop's
/// components/sync/cloudflare_oauth.h): OAuth 2.0 authorization code with
/// PKCE (S256), Compass's public client, no client secret. The access token
/// is used once and never stored; the client may not ask for
/// offline_access, so there is no refresh token either.
public enum CloudflareOAuth {
  public static let clientID = "9a35ed0a6094c850c6b4eb2835af9d37"
  /// Never loaded: the sign-in web view catches the navigation to it and
  /// takes the code from it.
  public static let redirectURI = "https://compassbrowser.org/oauth/cloudflare"
  public static let authorizeURL = "https://dash.cloudflare.com/oauth2/auth"
  public static let tokenURL = URL(string: "https://dash.cloudflare.com/oauth2/token")!
  /// What the deployer needs: the account, and editing Workers.
  public static let scopes = "account-settings.read workers-scripts.admin"

  /// One sign-in's secrets: the PKCE verifier and the state.
  public struct Request: Sendable, Equatable, Identifiable {
    public var verifier: String
    public var state: String
    /// The page to open.
    public var url: URL
    public var id: String { state }
  }

  /// A new request: a 64-character verifier (its SHA-256 the challenge)
  /// and a 32-character state, both base64url.
  public static func makeRequest() -> Request {
    let verifier = randomBase64URL(48)
    let state = randomBase64URL(24)  // Cloudflare wants 8 or more.
    var c = URLComponents(string: authorizeURL)!
    c.percentEncodedQuery = [
      ("response_type", "code"),
      ("client_id", clientID),
      ("redirect_uri", redirectURI),
      ("scope", scopes),
      ("state", state),
      ("code_challenge", challenge(verifier)),
      ("code_challenge_method", "S256"),
    ].map { formEscape($0.0) + "=" + formEscape($0.1) }.joined(separator: "&")
    return Request(verifier: verifier, state: state, url: c.url!)
  }

  /// base64url(SHA-256(verifier)), unpadded.
  static func challenge(_ verifier: String) -> String {
    Data(SHA256.hash(data: Data(verifier.utf8))).base64URLEncodedString(padded: false)
  }

  /// Whether `url` is the redirect URI (whatever its query).
  public static func isRedirect(_ url: URL) -> Bool {
    guard let c = URLComponents(url: url, resolvingAgainstBaseURL: false),
      let r = URLComponents(string: redirectURI)
    else { return false }
    return c.scheme?.lowercased() == r.scheme && c.host?.lowercased() == r.host
      && (c.port ?? 443) == 443 && c.path == r.path
  }

  /// What the redirect brought: the code, or the server's error
  /// ("access_denied"...), or "state" when the state doesn't match.
  public struct Reply: Sendable, Equatable {
    public var code: String?
    public var error: String
  }

  public static func parseRedirect(_ url: URL, expectedState: String) -> Reply {
    let items = formItems(url)
    func value(_ name: String) -> String {
      items.first { $0.name == name }?.value ?? ""
    }
    if expectedState.isEmpty || value("state") != expectedState { return Reply(code: nil, error: "state") }
    if !value("error").isEmpty { return Reply(code: nil, error: value("error")) }
    if value("code").isEmpty { return Reply(code: nil, error: "no_code") }
    return Reply(code: value("code"), error: "")
  }

  /// Trades the code for an access token at the token endpoint; nil on any
  /// refusal or failure.
  public static func exchange(
    code: String, verifier: String, tokenURL: URL = tokenURL, session: URLSession = .shared
  ) async -> String? {
    var request = URLRequest(url: tokenURL, timeoutInterval: 30)
    request.httpMethod = "POST"
    request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
    request.httpShouldHandleCookies = false
    request.httpBody = Data(
      [
        ("grant_type", "authorization_code"),
        ("code", code),
        ("redirect_uri", redirectURI),
        ("client_id", clientID),
        ("code_verifier", verifier),
      ].map { $0.0 + "=" + formEscape($0.1) }.joined(separator: "&").utf8)
    guard let (data, response) = try? await session.data(for: request),
      (response as? HTTPURLResponse)?.statusCode == 200,
      let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
      let token = json["access_token"] as? String, !token.isEmpty
    else { return nil }
    return token
  }

  /// The query's items, read as a form ("+" is a space), as Chromium's
  /// GetValueForKeyInQuery().
  static func formItems(_ url: URL) -> [URLQueryItem] {
    guard var c = URLComponents(url: url, resolvingAgainstBaseURL: false) else { return [] }
    let query = c.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%20")
    c.percentEncodedQuery = query
    return c.queryItems ?? []
  }

  /// A form value as Chromium's EscapeQueryParamValue(use_plus: true):
  /// RFC 3986's unreserved characters as they are, a space as "+".
  static func formEscape(_ value: String) -> String {
    value.split(separator: " ", omittingEmptySubsequences: false)
      .map { ServerEndpoint.escape(String($0)) }.joined(separator: "+")
  }

  static func randomBase64URL(_ bytes: Int) -> String {
    var buffer = [UInt8](repeating: 0, count: bytes)
    precondition(SecRandomCopyBytes(kSecRandomDefault, bytes, &buffer) == errSecSuccess)
    return Data(buffer).base64URLEncodedString(padded: false)
  }
}
