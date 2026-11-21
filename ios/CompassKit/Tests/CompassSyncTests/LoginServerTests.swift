// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import Testing

/// Company sign-in against the real Go server with its development identity
/// provider (COMPASS_SYNC_DEV_FAKE_OIDC): the gate, the login with the
/// browser's part done by hand (as the ASWebAuthenticationSession would),
/// then sync, and sign-out.
@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
struct LoginServerTests {
  @Test func signInThenSync() async throws {
    let port = Int.random(in: 20000..<40000)
    let server = try await TestServer(
      environment: ["COMPASS_SYNC_DEV_FAKE_OIDC": "1", "COMPASS_SYNC_PUBLIC_HOST": "localhost:\(port)"], port: port)
    // Discovery runs in the background at start.
    var provider: String?
    for _ in 0..<50 {
      provider = try await LoginClient.info(serverURL: server.url)
      if provider != nil { break }
      try await Task.sleep(for: .milliseconds(100))
    }
    #expect(provider != nil, "\(server.log)")

    let chain = try Chain(seed: Chain.generateSeed())
    let device = await TestDevice(server: server.url, chain: chain, name: "iPhone", first: true)
    // Unbound: the gate's 401, told apart from a refused key.
    await #expect(throws: SyncEngineError.connection(.loginRequired)) { try await device.sync() }
    let client = LoginClient(serverURL: server.url, authKey: chain.authKey)
    #expect(try await client.account() == .init(loginRequired: true, user: nil, expires: nil))

    var request: LoginClient.Request?
    for _ in 0..<50 {  // 503 login_unavailable until the provider is discovered
      do {
        request = try await client.start(deviceName: "Test iPhone")
        break
      } catch LoginClient.LoginError.unavailable {
        try await Task.sleep(for: .milliseconds(100))
      }
    }
    let login = try #require(request)
    #expect(try await client.poll(login.id, wait: false) == .pending)

    let callback = try await Browser().signIn(login.url, email: "ada@example.com", name: "Ada")
    #expect(callback.map(LoginClient.callbackStatus) == "done", "\(server.log)")
    #expect(try await client.poll(login.id) == .done(.init(name: "Ada", email: "ada@example.com")))
    let account = try await client.account()
    #expect(account.loginRequired == false)
    #expect(account.user?.email == "ada@example.com")

    try await device.sync()

    try await client.signOut()
    #expect(try await client.account().loginRequired)
  }
}

/// The browser's part: the server's start page, the fake identity
/// provider's form, the consent page's Allow; the compass: redirect at the
/// end is returned, not followed. Cookies by hand: Foundation's store
/// drops those of a dotless host such as localhost.
private final class Browser: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
  lazy var session: URLSession = {
    let config = URLSessionConfiguration.ephemeral
    config.httpCookieStorage = nil
    config.httpShouldSetCookies = false
    return URLSession(configuration: config, delegate: self, delegateQueue: nil)
  }()
  private let lock = NSLock()
  private var cookies: [String: String] = [:]

  private func keep(_ response: URLResponse) {
    guard let http = response as? HTTPURLResponse, let header = http.value(forHTTPHeaderField: "Set-Cookie")
    else { return }
    lock.withLock {
      for cookie in header.components(separatedBy: ", ") {
        let pair = cookie.split(separator: ";").first.map(String.init) ?? ""
        let parts = pair.split(separator: "=", maxSplits: 1).map(String.init)
        if parts.count == 2 { cookies[parts[0]] = parts[1] }
      }
    }
  }

  private func withCookies(_ request: URLRequest) -> URLRequest {
    var request = request
    let header = lock.withLock { cookies.map { "\($0.key)=\($0.value)" }.joined(separator: "; ") }
    if !header.isEmpty { request.setValue(header, forHTTPHeaderField: "Cookie") }
    return request
  }

  func urlSession(
    _ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
    newRequest request: URLRequest
  ) async -> URLRequest? {
    keep(response)
    return request.url?.scheme?.hasPrefix("http") == true ? withCookies(request) : nil
  }

  private func load(_ request: URLRequest) async throws -> (Data, URLResponse) {
    let (data, response) = try await session.data(for: withCookies(request))
    keep(response)
    return (data, response)
  }

  func signIn(_ start: URL, email: String, name: String) async throws -> URL? {
    // start → 302 → the identity provider's form.
    let (formPage, formResponse) = try await load(URLRequest(url: start))
    var fields = Self.hiddenInputs(String(decoding: formPage, as: UTF8.self))
    fields["email"] = email
    fields["name"] = name
    fields["email_verified"] = "true"
    // Its submit → 302 → the server's callback → the consent page.
    let (consent, consentResponse) = try await post(formResponse.url!, fields)
    let html = String(decoding: consent, as: UTF8.self)
    guard let action = Self.match(#"action="([^"]*/approve)""#, html),
      let csrf = Self.hiddenInputs(html)["csrf"]
    else {
      Issue.record("no consent form: \(html)")
      return nil
    }
    // Allow → 302 compass://sync-login-done?status=done.
    let (_, done) = try await post(URL(string: action, relativeTo: consentResponse.url)!, ["csrf": csrf])
    guard let http = done as? HTTPURLResponse, http.statusCode == 302 else { return nil }
    return http.value(forHTTPHeaderField: "Location").flatMap(URL.init(string:))
  }

  private func post(_ url: URL, _ fields: [String: String]) async throws -> (Data, URLResponse) {
    var request = URLRequest(url: url)
    request.httpMethod = "POST"
    request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
    var c = URLComponents()
    c.queryItems = fields.map { URLQueryItem(name: $0.key, value: $0.value) }
    request.httpBody = Data((c.percentEncodedQuery ?? "").replacingOccurrences(of: "+", with: "%2B").utf8)
    return try await load(request)
  }

  static func hiddenInputs(_ html: String) -> [String: String] {
    var out: [String: String] = [:]
    let regex = try! NSRegularExpression(pattern: #"<input type="hidden" name="([^"]*)" value="([^"]*)">"#)
    for m in regex.matches(in: html, range: NSRange(html.startIndex..., in: html)) {
      let name = String(html[Range(m.range(at: 1), in: html)!])
      out[name] = unescape(String(html[Range(m.range(at: 2), in: html)!]))
    }
    return out
  }

  static func match(_ pattern: String, _ html: String) -> String? {
    let regex = try! NSRegularExpression(pattern: pattern)
    guard let m = regex.firstMatch(in: html, range: NSRange(html.startIndex..., in: html)) else { return nil }
    return unescape(String(html[Range(m.range(at: 1), in: html)!]))
  }

  /// html/template's escapes in attribute values.
  static func unescape(_ s: String) -> String {
    s.replacingOccurrences(of: "&#43;", with: "+").replacingOccurrences(of: "&#34;", with: "\"")
      .replacingOccurrences(of: "&#39;", with: "'").replacingOccurrences(of: "&lt;", with: "<")
      .replacingOccurrences(of: "&gt;", with: ">").replacingOccurrences(of: "&amp;", with: "&")
  }
}
