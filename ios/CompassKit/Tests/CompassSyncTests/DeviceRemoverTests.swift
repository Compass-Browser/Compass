// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import SyncProto
import Testing

/// Serves canned replies and records the requests (the device removal
/// tests; serialized, as the state is shared).
final class SyncStubProtocol: URLProtocol, @unchecked Sendable {
  struct Reply {
    var status: Int
    var headers: [String: String] = [:]
    var body = ""
  }
  nonisolated(unsafe) static var replies: [Reply] = []
  nonisolated(unsafe) static var requests: [URLRequest] = []

  static func reset(_ replies: [Reply]) {
    self.replies = replies
    requests = []
  }

  static var session: URLSession {
    let config = URLSessionConfiguration.ephemeral
    config.protocolClasses = [SyncStubProtocol.self]
    return URLSession(configuration: config)
  }

  override class func canInit(with request: URLRequest) -> Bool { true }
  override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
  override func startLoading() {
    Self.requests.append(request)
    let reply = Self.replies.isEmpty ? Reply(status: 500) : Self.replies.removeFirst()
    let response = HTTPURLResponse(
      url: request.url!, statusCode: reply.status, httpVersion: nil, headerFields: reply.headers)!
    client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
    client?.urlProtocol(self, didLoad: Data(reply.body.utf8))
    client?.urlProtocolDidFinishLoading(self)
  }
  override func stopLoading() {}
}

actor Flag {
  var set = false
  func raise() { set = true }
}

@Suite(.serialized)
struct DeviceRemoverTests {
  let server = URL(string: "https://sync.example.com/v2")!
  let key = Data(repeating: 7, count: 32)
  static let removedReply = SyncStubProtocol.Reply(
    status: 403, headers: ["X-Compass-Device": "removed", "Content-Type": "application/json"],
    body: #"{"error":"device_removed"}"#)

  func remover() -> DeviceRemover {
    DeviceRemover(serverURL: server, cacheGUID: "me+/=", authKey: key, session: SyncStubProtocol.session)
  }

  @Test func escapesTheGUIDs() {
    // Base64: "/" stays in the path segment and "+" in the query only as
    // escapes (a bare "+" reads as a space on the server).
    #expect(
      DeviceRemover.url(server, device: "a+b/c=", cacheGUID: "me+/=").absoluteString
        == "https://sync.example.com/v2/compass/devices/a%2Bb%2Fc%3D?client_id=me%2B%2F%3D")
    #expect(
      DeviceRemover.url(URL(string: "https://h/v2/")!, device: "x", cacheGUID: "y").absoluteString
        == "https://h/v2/compass/devices/x?client_id=y")
  }

  @Test func sendsASignedDelete() async throws {
    SyncStubProtocol.reset([.init(status: 204)])
    try await remover().remove("other/guid==")
    let request = try #require(SyncStubProtocol.requests.first)
    #expect(request.httpMethod == "DELETE")
    #expect(request.url?.absoluteString == "https://sync.example.com/v2/compass/devices/other%2Fguid%3D%3D?client_id=me%2B%2F%3D")
    let auth = try #require(request.value(forHTTPHeaderField: "Authorization"))
    #expect(auth.hasPrefix("Bearer ") && auth.count > 20)
  }

  @Test func mapsTheReplies() async {
    let cases: [(SyncStubProtocol.Reply, DeviceRemover.Failure)] = [
      (.init(status: 409, body: #"{"error":"this_device"}"#), .thisDevice),
      (Self.removedReply, .deviceRemoved),
      (.init(status: 401, headers: ["X-Compass-Login": "required"], body: #"{"error":"login_required"}"#), .loginRequired),
      (.init(status: 401), .http(401)),
      (.init(status: 400, body: #"{"error":"bad_request"}"#), .http(400)),
      // A reset chain: refused, but not this device's removal.
      (.init(status: 403, body: #"{"error":"chain_disabled"}"#), .http(403)),
    ]
    for (reply, failure) in cases {
      SyncStubProtocol.reset([reply])
      await #expect(throws: failure) { try await remover().remove("x") }
    }
    // Already gone: done all the same.
    SyncStubProtocol.reset([.init(status: 204)])
    await #expect(throws: Never.self) { try await remover().remove("x") }
  }

  static var message: SyncPb_ClientToServerMessage {
    var message = SyncPb_ClientToServerMessage()
    message.share = ""
    message.messageContents = .getUpdates
    return message
  }

  @Test func commandSeesTheRemoval() async throws {
    let connection = HTTPSyncConnection(
      serverURL: server, cacheGUID: "me", authKey: key, session: SyncStubProtocol.session)
    SyncStubProtocol.reset([Self.removedReply])
    await #expect(throws: SyncConnectionError.deviceRemoved) { _ = try await connection.post(Self.message) }
    // A 403 without the header is another refusal.
    SyncStubProtocol.reset([.init(status: 403, body: #"{"error":"chain_disabled"}"#)])
    await #expect(throws: SyncConnectionError.http(403)) { _ = try await connection.post(Self.message) }

    // Through the engine: a connection error the app tells apart.
    SyncStubProtocol.reset([Self.removedReply])
    let engine = SyncEngine(connection: connection, passphrase: "p", store: MemorySyncMetadataStore())
    await #expect(throws: SyncEngineError.connection(.deviceRemoved)) { try await engine.sync() }
  }

  @Test func pushSeesTheRemoval() async {
    SyncStubProtocol.reset([Self.removedReply])
    await #expect(throws: SyncConnectionError.deviceRemoved) {
      try await Push.register(
        token: Data([1]), environment: .sandbox, server: server, cacheGUID: "me", authKey: key,
        session: SyncStubProtocol.session)
    }
    #expect(SyncStubProtocol.requests.first?.url?.query == "client_id=me")
  }

  @Test func notifierStopsWhenRemoved() async {
    SyncStubProtocol.reset([Self.removedReply])
    let notifier = SyncNotifier(
      serverURL: server, cacheGUID: "me+1", authKey: key, protocolClasses: [SyncStubProtocol.self])
    let removed = Flag()
    // Returns by itself: no retries after a removal.
    await notifier.run(changed: { _ in }, removed: { await removed.raise() })
    #expect(await removed.set)
    #expect(SyncStubProtocol.requests.count == 1)
    #expect(SyncStubProtocol.requests.first?.url?.absoluteString == "https://sync.example.com/v2/compass/events?client_id=me%2B1")
  }
}
