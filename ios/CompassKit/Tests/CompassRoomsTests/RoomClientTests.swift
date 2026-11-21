// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation
import Testing

@testable import CompassRooms

/// Canned replies; records requests with their bodies.
final class RoomStubProtocol: URLProtocol, @unchecked Sendable {
  struct Reply {
    var status: Int
    var body = ""
  }
  nonisolated(unsafe) static var replies: [Reply] = []
  nonisolated(unsafe) static var requests: [(URLRequest, Data)] = []

  static func reset(_ replies: [Reply]) {
    self.replies = replies
    requests = []
  }

  static var session: URLSession {
    let config = URLSessionConfiguration.ephemeral
    config.protocolClasses = [RoomStubProtocol.self]
    return URLSession(configuration: config)
  }

  override class func canInit(with request: URLRequest) -> Bool { true }
  override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
  override func startLoading() {
    var body = request.httpBody ?? Data()
    if body.isEmpty, let stream = request.httpBodyStream {
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
      url: request.url!, statusCode: reply.status, httpVersion: nil,
      headerFields: ["Content-Type": "application/json"])!
    client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
    client?.urlProtocol(self, didLoad: Data(reply.body.utf8))
    client?.urlProtocolDidFinishLoading(self)
  }
  override func stopLoading() {}

  static func json(_ index: Int) -> [String: Any] {
    (try? JSONSerialization.jsonObject(with: requests[index].1)) as? [String: Any] ?? [:]
  }
}

@Suite(.serialized)
struct RoomClientTests {
  let server = URL(string: "https://sync.example.com/v2")!
  let key = Data(repeating: 7, count: 32)
  let roomID = "n4bQgYhMfWWaL-qgxVrQFQ"
  let member = String(repeating: "ab", count: 32)

  var client: RoomClient {
    RoomClient(serverURL: server, authKey: key, cacheGUID: "guid+/=", session: RoomStubProtocol.session)
  }

  static let roomJSON = """
    {"id":"n4bQgYhMfWWaL-qgxVrQFQ","kind":"folder","owner":"\(String(repeating: "ab", count: 32))",
     "created":1793491200000,"epoch":2,"seq":42,"bytes":10,"role":"editor","pending":false,
     "members":[{"client_id":"\(String(repeating: "ab", count: 32))","role":"owner","identity_pub":"\(Data(repeating: 1, count: 32).base64EncodedString())",
       "profile":null,"name":"Ada","email":"ada@example.com","invited":1,"joined":2,"pending":false}],
     "envelopes":[{"epoch":1,"sealed":"\(Data(repeating: 2, count: 80).base64EncodedString())"}],
     "invites":[{"id":"inv","role":"viewer","created":1,"expires":0,"uses_left":null,"revoked":false}]}
    """

  @Test func readsARoom() async throws {
    RoomStubProtocol.reset([.init(status: 200, body: Self.roomJSON)])
    let info = try await client.room(roomID)
    #expect(info.epoch == 2 && info.seq == 42 && info.role == .editor)
    #expect(info.members.first?.name == "Ada")
    #expect(info.members.first?.identityPub == Data(repeating: 1, count: 32))
    #expect(info.envelopes.first?.sealed.count == 80)
    #expect(info.invites?.first?.usesLeft == nil)
    let (request, _) = RoomStubProtocol.requests[0]
    #expect(request.httpMethod == "GET")
    // The cache GUID is escaped; the path is the room's.
    #expect(request.url?.absoluteString == "https://sync.example.com/v2/compass/rooms/\(roomID)?client_id=guid%2B%2F%3D")
    let auth = request.value(forHTTPHeaderField: "Authorization") ?? ""
    #expect(auth.hasPrefix("Bearer "))
  }

  @Test func refusesIDsThatCouldEscapeThePath() async throws {
    RoomStubProtocol.reset([])
    for bad in ["../identity", "n4bQgYhMfWWaL-qgxVrQF", "n4bQgYhMfWWaL-qgxVrQFQ?x", "n4bQgYhMfWWaL/qgxVrQFQ"] {
      await #expect(throws: RoomError.badArgument("room ID")) { try await client.room(bad) }
    }
    await #expect(throws: RoomError.badArgument("client ID")) {
      try await client.removeMember(roomID, member: "../../x")
    }
    await #expect(throws: RoomError.badArgument("invite ID")) {
      try await client.revokeInvite(roomID, invite: "a/b")
    }
    #expect(RoomStubProtocol.requests.isEmpty)
  }

  @Test func writesRecordsWithBase64AndBaseSeq() async throws {
    RoomStubProtocol.reset([.init(status: 200, body: #"{"seq":44,"records":[{"id":"a","seq":43},{"id":"b","seq":44}]}"#)])
    let result = try await client.write(
      roomID,
      records: [
        RoomWrite(id: "a", epoch: 2, ciphertext: Data([0xff, 0xfe, 0x00]), baseSeq: 0),
        RoomWrite(id: "b", epoch: 2, ciphertext: nil, deleted: true, baseSeq: 7),
      ])
    #expect(result.records.map(\.seq) == [43, 44])
    let body = RoomStubProtocol.json(0)
    let records = body["records"] as? [[String: Any]] ?? []
    #expect(records[0]["ciphertext"] as? String == "//4A")  // standard base64, padded
    #expect(records[0]["base_seq"] as? Int == 0)
    #expect(records[1]["deleted"] as? Bool == true)
    #expect(records[1]["ciphertext"] == nil)
    #expect(RoomStubProtocol.requests[0].0.url?.path == "/v2/compass/rooms/\(roomID)/records")
  }

  @Test func mapsErrors() async throws {
    let stored = #"{"id":"a","seq":9,"epoch":2,"deleted":false,"author":"x","ciphertext":"AAAA"}"#
    RoomStubProtocol.reset([
      .init(status: 409, body: #"{"error":"conflict","current":[\#(stored)]}"#),
      .init(status: 409, body: #"{"error":"stale_epoch"}"#),
      .init(status: 403, body: #"{"error":"removed"}"#),
      .init(status: 404, body: #"{"error":"not_found"}"#),
      .init(status: 403, body: #"{"error":"invite_expired"}"#),
    ])
    let write = RoomWrite(id: "a", epoch: 2, ciphertext: Data(count: 28), baseSeq: 3)
    await #expect(throws: RoomError.conflict([RoomRecord(id: "a", seq: 9, epoch: 2, deleted: false, author: "x", ciphertext: Data([0, 0, 0]))])) {
      try await client.write(roomID, records: [write])
    }
    await #expect(throws: RoomError.staleEpoch) { try await client.write(roomID, records: [write]) }
    await #expect(throws: RoomError.removed) { try await client.room(roomID) }
    await #expect(throws: RoomError.notFound) { try await client.room(roomID) }
    do {
      _ = try await client.join(roomID, inviteID: "inv", secret: Data(count: 32), identityPub: Data(count: 32))
      Issue.record("joined")
    } catch let error as RoomError {
      #expect(error.code == "invite_expired")
      #expect(error.description == "This invite link has expired.")
    }
  }

  @Test func createSendsTheChosenIDAndEnvelope() async throws {
    RoomStubProtocol.reset([.init(status: 409, body: #"{"error":"room_exists"}"#), .init(status: 201, body: Self.roomJSON)])
    let id = RoomInviteLink.unb64(roomID)!
    await #expect(throws: RoomError.roomExists) {
      try await client.createRoom(id: id, kind: .folder, identityPub: Data(count: 32), envelope: Data(count: 80))
    }
    _ = try await client.createRoom(id: id, kind: .folder, identityPub: Data(count: 32), envelope: Data(repeating: 5, count: 80))
    let body = RoomStubProtocol.json(1)
    #expect(body["id"] as? String == roomID && body["kind"] as? String == "folder")
    #expect(body["envelope"] as? String == Data(repeating: 5, count: 80).base64EncodedString())
    await #expect(throws: RoomError.badArgument("room ID")) {
      try await client.createRoom(id: Data(count: 15), kind: .folder, identityPub: Data(count: 32), envelope: Data(count: 80))
    }
  }

  @Test func joinSendsTheSecretAsBase64URL() async throws {
    RoomStubProtocol.reset([.init(status: 200, body: Self.roomJSON)])
    let secret = Data((0..<32).map { UInt8($0 * 8) })
    _ = try await client.join(roomID, inviteID: "inv_1-x", secret: secret, identityPub: Data(repeating: 3, count: 32))
    let body = RoomStubProtocol.json(0)
    #expect(body["secret"] as? String == secret.base64URLEncodedString(padded: false))
    #expect(body["invite_id"] as? String == "inv_1-x")
    #expect(body["identity_pub"] as? String == Data(repeating: 3, count: 32).base64EncodedString())
  }

  @Test func changesAndInvitesQueries() async throws {
    RoomStubProtocol.reset([
      .init(status: 200, body: #"{"seq":3,"epoch":1,"records":[{"id":"x","seq":3,"epoch":1,"deleted":true,"author":"a"}],"more":false}"#),
      .init(status: 201, body: #"{"id":"inv","role":"editor","created":1,"expires":5000,"uses_left":3,"revoked":false}"#),
    ])
    let changes = try await client.changes(roomID, since: 2, wait: 60)
    #expect(changes.records.first?.deleted == true && changes.records.first?.ciphertext == nil)
    let query = RoomStubProtocol.requests[0].0.url?.query ?? ""
    #expect(query == "since=2&wait=30&client_id=guid%2B%2F%3D")
    let invite = try await client.createInvite(
      roomID, role: .editor, secretHash: String(repeating: "0", count: 64), expires: Date(timeIntervalSince1970: 5),
      uses: 3)
    #expect(invite.usesLeft == 3)
    let body = RoomStubProtocol.json(1)
    #expect(body["expires"] as? Int == 5000 && body["uses"] as? Int == 3 && body["role"] as? String == "editor")
  }

  @Test func rotationAndEnvelopes() async throws {
    RoomStubProtocol.reset([.init(status: 204), .init(status: 204), .init(status: 204)])
    try await client.rotate(roomID, epoch: 3, envelopes: [member: Data(repeating: 9, count: 80)])
    let rotate = RoomStubProtocol.json(0)
    #expect(rotate["epoch"] as? Int == 3)
    #expect((rotate["envelopes"] as? [[String: Any]])?.first?["client_id"] as? String == member)
    try await client.addEnvelopes(roomID, [RoomEnvelopeFor(clientID: member, epoch: 1, sealed: Data(count: 80))])
    try await client.setProfile(roomID, member: member, profile: nil)
    #expect(RoomStubProtocol.json(2)["profile"] is NSNull)
    #expect(RoomStubProtocol.requests[2].0.httpMethod == "PATCH")
  }
}
