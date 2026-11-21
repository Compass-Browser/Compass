// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import SyncProto
import Testing

/// Serves canned responses and records requests (with their bodies, which
/// URLProtocol only sees as a stream).
final class LoginStubProtocol: URLProtocol, @unchecked Sendable {
  struct Reply {
    var status: Int
    var headers: [String: String] = ["Content-Type": "application/json"]
    var body: String = ""
  }
  nonisolated(unsafe) static var replies: [Reply] = []
  nonisolated(unsafe) static var requests: [(URLRequest, Data)] = []

  static func session() -> URLSession {
    replies = []
    requests = []
    let config = URLSessionConfiguration.ephemeral
    config.protocolClasses = [LoginStubProtocol.self]
    return URLSession(configuration: config)
  }

  override class func canInit(with request: URLRequest) -> Bool { true }
  override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
  override func startLoading() {
    var body = request.httpBody ?? Data()
    if let stream = request.httpBodyStream {
      stream.open()
      var buffer = [UInt8](repeating: 0, count: 4096)
      while stream.hasBytesAvailable {
        let n = stream.read(&buffer, maxLength: buffer.count)
        if n <= 0 { break }
        body.append(buffer, count: n)
      }
      stream.close()
    }
    Self.requests.append((request, body))
    let reply = Self.replies.isEmpty ? Reply(status: 500) : Self.replies.removeFirst()
    let response = HTTPURLResponse(
      url: request.url!, statusCode: reply.status, httpVersion: nil, headerFields: reply.headers)!
    client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
    client?.urlProtocol(self, didLoad: Data(reply.body.utf8))
    client?.urlProtocolDidFinishLoading(self)
  }
  override func stopLoading() {}
}

@Suite(.serialized) struct LoginClientTests {
  let server = URL(string: "https://sync.example.com/v2")!
  let chain = try! Chain(seed: Data(0..<16))

  func client() -> LoginClient {
    LoginClient(serverURL: server, authKey: chain.authKey, session: LoginStubProtocol.session())
  }

  /// The chain ID a request's bearer token carries (upper-case hex pubkey).
  func signer(_ request: URLRequest) -> String? {
    guard let header = request.value(forHTTPHeaderField: "Authorization"), header.hasPrefix("Bearer ") else {
      return nil
    }
    return signer(token: String(header.dropFirst(7)))
  }

  func signer(token: String) -> String? {
    let b64 = token.replacingOccurrences(of: "-", with: "+")
      .replacingOccurrences(of: "_", with: "/")
    guard let data = Data(base64Encoded: b64), let plain = String(data: data, encoding: .utf8) else { return nil }
    return plain.split(separator: "|").last.map(String.init)
  }

  @Test func infoWithAndWithoutLogin() async throws {
    let session = LoginStubProtocol.session()
    LoginStubProtocol.replies = [
      .init(status: 200, body: #"{"login":{"type":"oidc","name":"Acme SSO"}}"#),
      .init(status: 200, body: #"{"login":null}"#),
    ]
    #expect(try await LoginClient.info(serverURL: server, session: session) == "Acme SSO")
    #expect(try await LoginClient.info(serverURL: server, session: session) == nil)
    let request = LoginStubProtocol.requests[0].0
    #expect(request.url?.absoluteString == "https://sync.example.com/v2/compass/info")
    #expect(request.value(forHTTPHeaderField: "Authorization") == nil)
  }

  @Test func infoWithATrailingSlash() async throws {
    let session = LoginStubProtocol.session()
    LoginStubProtocol.replies = [.init(status: 200, body: #"{"login":null}"#)]
    _ = try await LoginClient.info(serverURL: URL(string: "http://localhost:8295/v2/")!, session: session)
    #expect(LoginStubProtocol.requests[0].0.url?.absoluteString == "http://localhost:8295/v2/compass/info")
  }

  @Test func accountSignedIn() async throws {
    let c = client()
    LoginStubProtocol.replies = [
      .init(
        status: 200,
        body: #"{"login_required":false,"user":{"name":"Ada","email":"ada@acme.com"},"expires":1700000000000}"#)
    ]
    let account = try await c.account()
    #expect(account.loginRequired == false)
    #expect(account.user == .init(name: "Ada", email: "ada@acme.com"))
    #expect(account.expires == Date(timeIntervalSince1970: 1_700_000_000))
    let request = LoginStubProtocol.requests[0].0
    #expect(request.httpMethod == "GET")
    #expect(request.url?.absoluteString == "https://sync.example.com/v2/compass/account")
    #expect(signer(request) == Ed25519.publicKey(fromSeed: chain.authKey).hexString(uppercase: true))
  }

  @Test func accountLoginRequiredAndNoExpiry() async throws {
    let c = client()
    LoginStubProtocol.replies = [
      .init(status: 200, body: #"{"login_required":true,"user":null,"expires":0}"#),
      .init(status: 200, body: #"{"login_required":false,"user":{"name":"Ada","email":"a@b.c"},"expires":0}"#),
    ]
    #expect(try await c.account() == .init(loginRequired: true, user: nil, expires: nil))
    #expect(try await c.account().expires == nil)
  }

  @Test func accountRefusedKey() async throws {
    let c = client()
    LoginStubProtocol.replies = [.init(status: 401, body: #"{"error":"invalid sync token"}"#)]
    await #expect(throws: LoginClient.LoginError.unauthorized) { try await c.account() }
  }

  @Test func startSendsDeviceNameAndReturn() async throws {
    let c = client()
    LoginStubProtocol.replies = [
      .init(
        status: 200,
        body: #"{"id":"abc123","url":"https://sync.example.com/v2/compass/login/abc123/start","user_code":"K7F-2QM","expires":1700000600000}"#
      )
    ]
    let request = try await c.start(deviceName: "Ada's iPhone")
    #expect(request.id == "abc123")
    #expect(request.url.absoluteString == "https://sync.example.com/v2/compass/login/abc123/start")
    #expect(request.userCode == "K7F-2QM")
    #expect(request.expires == Date(timeIntervalSince1970: 1_700_000_600))
    let (sent, body) = LoginStubProtocol.requests[0]
    #expect(sent.httpMethod == "POST")
    #expect(sent.url?.absoluteString == "https://sync.example.com/v2/compass/login")
    #expect(sent.value(forHTTPHeaderField: "Content-Type") == "application/json")
    let json = try JSONSerialization.jsonObject(with: body) as? [String: String]
    #expect(json == ["device_name": "Ada's iPhone", "return": "compass"])
  }

  @Test func startErrors() async throws {
    let c = client()
    LoginStubProtocol.replies = [
      .init(status: 404, body: #"{"error":"login_disabled"}"#),
      .init(status: 503, body: #"{"error":"login_unavailable"}"#),
      .init(status: 429, body: #"{"error":"too many login requests"}"#),
      .init(status: 200, body: #"{"id":"x"}"#),
    ]
    await #expect(throws: LoginClient.LoginError.disabled) { try await c.start(deviceName: "x") }
    await #expect(throws: LoginClient.LoginError.unavailable) { try await c.start(deviceName: "x") }
    await #expect(throws: LoginClient.LoginError.rateLimited) { try await c.start(deviceName: "x") }
    await #expect(throws: LoginClient.LoginError.badReply) { try await c.start(deviceName: "x") }
  }

  @Test func pollStatuses() async throws {
    let c = client()
    LoginStubProtocol.replies = [
      .init(status: 200, body: #"{"status":"pending","user":null}"#),
      .init(status: 200, body: #"{"status":"done","user":{"name":"Ada","email":"ada@acme.com"}}"#),
      .init(status: 200, body: #"{"status":"denied","user":null}"#),
      .init(status: 200, body: #"{"status":"expired","user":null}"#),
      .init(status: 404, body: #"{"error":"not_found"}"#),
      .init(status: 200, body: #"{"status":"pending"}"#),
    ]
    #expect(try await c.poll("abc") == .pending)
    #expect(try await c.poll("abc") == .done(.init(name: "Ada", email: "ada@acme.com")))
    #expect(try await c.poll("abc") == .denied)
    #expect(try await c.poll("abc") == .expired)
    #expect(try await c.poll("abc") == .expired)  // unknown: a restart dropped it
    #expect(try await c.poll("abc", wait: false) == .pending)
    let urls = LoginStubProtocol.requests.map { $0.0.url?.absoluteString }
    #expect(urls[0] == "https://sync.example.com/v2/compass/login/abc")
    #expect(urls[5] == "https://sync.example.com/v2/compass/login/abc?wait=0")
    #expect(LoginStubProtocol.requests.allSatisfy { signer($0.0) != nil })
  }

  @Test func signOutAndLink() async throws {
    let c = client()
    LoginStubProtocol.replies = [.init(status: 204), .init(status: 204), .init(status: 401, body: #"{"error":"login_required"}"#)]
    try await c.signOut()
    let profileKey = chain.authKey(profileUUID: "2b5c3e1a-0000-4000-8000-000000000001")
    try await c.link(profileAuthKey: profileKey)
    await #expect(throws: LoginClient.LoginError.unauthorized) { try await c.link(profileAuthKey: profileKey) }
    let signOut = LoginStubProtocol.requests[0].0
    #expect(signOut.httpMethod == "DELETE")
    #expect(signOut.url?.absoluteString == "https://sync.example.com/v2/compass/account")
    let link = LoginStubProtocol.requests[1].0
    #expect(link.httpMethod == "POST")
    #expect(link.url?.absoluteString == "https://sync.example.com/v2/compass/account/link")
    #expect(signer(link) == Ed25519.publicKey(fromSeed: chain.authKey).hexString(uppercase: true))
    let linked = link.value(forHTTPHeaderField: "X-Compass-Linked-Token").flatMap { signer(token: $0) }
    #expect(linked == Ed25519.publicKey(fromSeed: profileKey).hexString(uppercase: true))
  }

  @Test func callbackURL() {
    #expect(LoginClient.callbackStatus(URL(string: "compass://sync-login-done?status=done")!) == "done")
    #expect(LoginClient.callbackStatus(URL(string: "compass://sync-login-done?status=denied")!) == "denied")
    #expect(LoginClient.isCallback(URL(string: "compass://sync-login-done")!))
    #expect(!LoginClient.isCallback(URL(string: "compass://search")!))
    #expect(!LoginClient.isCallback(URL(string: "https://sync-login-done/")!))
  }

  /// /command/ and push: the gate's 401 is told apart from a refused key.
  @Test func gated401sAreLoginRequired() async throws {
    let session = LoginStubProtocol.session()
    LoginStubProtocol.replies = [
      .init(status: 401, headers: ["X-Compass-Login": "required"], body: #"{"error":"login_required"}"#),
      .init(status: 401, body: "invalid token"),
      .init(status: 401, headers: ["X-Compass-Login": "required"], body: #"{"error":"login_required"}"#),
    ]
    var message = SyncPb_ClientToServerMessage()
    message.share = ""
    message.messageContents = .getUpdates
    let connection = HTTPSyncConnection(
      serverURL: server, cacheGUID: "guid", authKey: chain.authKey, session: session)
    await #expect(throws: SyncConnectionError.loginRequired) {
      try await connection.post(message)
    }
    await #expect(throws: SyncConnectionError.http(401)) {
      try await connection.post(message)
    }
    await #expect(throws: SyncConnectionError.loginRequired) {
      try await Push.register(
        token: Data([1, 2]), environment: .sandbox, server: server, cacheGUID: "guid", authKey: chain.authKey,
        session: session)
    }
  }
}
