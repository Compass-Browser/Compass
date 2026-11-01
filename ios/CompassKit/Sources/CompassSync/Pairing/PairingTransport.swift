// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation

/// Why a slot closed: DELETE's ?reason= and the server's 410 replies.
public enum PairingClosedReason: String, Sendable {
  case mismatch, cancel, deny, expired
}

public struct PairingMessage: Sendable, Equatable {
  public let seq: Int
  public let data: Data
  public init(seq: Int, data: Data) {
    self.seq = seq
    self.data = data
  }
}

/// A reply from the pairing endpoints: the HTTP status (0 when there was no
/// usable reply) and, with a 410, why the slot closed.
public struct PairingReply<Value: Sendable>: Sendable {
  public var status: Int
  public var closed: PairingClosedReason?
  public var value: Value?

  public init(status: Int, closed: PairingClosedReason? = nil, value: Value? = nil) {
    self.status = status
    self.closed = closed
    self.value = value
  }

  var isSuccess: Bool { status == 200 || status == 201 }
}

public struct PairingJoinInfo: Sendable {
  /// The name both sides bind into SPAKE2; empty from older servers.
  public var server: String
  /// The slot's remaining life; 0 from older servers.
  public var expiresIn: TimeInterval
}

public struct PairingOpenInfo: Sendable {
  public var slot: Int
  public var expiresIn: TimeInterval
  public var server: String
}

/// The server's pairing endpoints (sync.md, "Pairing API").
public protocol PairingTransport: Sendable {
  /// A: POST /compass/pair (signed).
  func open() async -> PairingReply<PairingOpenInfo>
  /// B: POST /compass/pair/{slot}/join. Keeps the b_token for B's later requests.
  func join(slot: Int) async -> PairingReply<PairingJoinInfo>
  /// POST /compass/pair/{slot}/msg.
  func send(slot: Int, from: PairingRole, data: Data) async -> PairingReply<Int>
  /// GET /compass/pair/{slot}/msg?for=<reader>&after=<after>, a long poll.
  func receive(slot: Int, reader: PairingRole, after: Int) async -> PairingReply<[PairingMessage]>
  /// DELETE /compass/pair/{slot}?reason=<reason>. Best effort.
  func close(slot: Int, by: PairingRole, reason: PairingClosedReason) async
}

/// PairingTransport over URLSession, as the desktop's
/// pairing_network_transport.cc.
public final class NetworkPairingTransport: PairingTransport, @unchecked Sendable {
  private let serverURL: URL
  private let session: URLSession
  /// A's credentials (a chain's auth key); nil on B.
  private let authKey: Data?
  private let lock = NSLock()
  private var bToken: String?

  public init(serverURL: URL, authKey: Data? = nil, session: URLSession = .shared) {
    self.serverURL = serverURL
    self.authKey = authKey
    self.session = session
  }

  /// <server path>/compass/pair[/suffix], without query or fragment.
  static func pairURL(_ server: URL, _ suffix: String) -> URL {
    var c = URLComponents(url: server, resolvingAgainstBaseURL: false)!
    var path = c.path
    while path.hasSuffix("/") { path.removeLast() }
    path += "/compass/pair"
    if !suffix.isEmpty { path += "/" + suffix }
    c.path = path
    c.query = nil
    c.fragment = nil
    return c.url!
  }

  public func open() async -> PairingReply<PairingOpenInfo> {
    let (status, json) = await request("POST", Self.pairURL(serverURL, ""), role: .a, body: [:])
    var reply = PairingReply<PairingOpenInfo>(status: status, closed: closedReason(status, json))
    if reply.isSuccess {
      guard let slot = json?["slot"] as? Int, let expires = json?["expires_in"] as? Int else {
        reply.status = 0
        return reply
      }
      reply.value = PairingOpenInfo(
        slot: slot, expiresIn: TimeInterval(expires), server: json?["server"] as? String ?? "")
    }
    return reply
  }

  public func join(slot: Int) async -> PairingReply<PairingJoinInfo> {
    let (status, json) = await request(
      "POST", Self.pairURL(serverURL, "\(slot)/join"), role: nil, body: [:])
    var reply = PairingReply<PairingJoinInfo>(status: status, closed: closedReason(status, json))
    if reply.isSuccess {
      guard let token = json?["b_token"] as? String, !token.isEmpty else {
        reply.status = 0
        return reply
      }
      lock.withLock { bToken = token }
      reply.value = PairingJoinInfo(
        server: json?["server"] as? String ?? "",
        expiresIn: TimeInterval(json?["expires_in"] as? Int ?? 0))
    }
    return reply
  }

  public func send(slot: Int, from: PairingRole, data: Data) async -> PairingReply<Int> {
    let (status, json) = await request(
      "POST", Self.pairURL(serverURL, "\(slot)/msg"), role: from,
      body: ["from": from.name, "data": data.base64EncodedString()])
    return PairingReply(status: status, closed: closedReason(status, json), value: json?["seq"] as? Int)
  }

  public func receive(slot: Int, reader: PairingRole, after: Int) async
    -> PairingReply<[PairingMessage]>
  {
    var c = URLComponents(
      url: Self.pairURL(serverURL, "\(slot)/msg"), resolvingAgainstBaseURL: false)!
    c.queryItems = [
      URLQueryItem(name: "for", value: reader.name), URLQueryItem(name: "after", value: "\(after)"),
    ]
    let (status, json) = await request("GET", c.url!, role: reader, body: nil, timeout: 40)
    var reply = PairingReply<[PairingMessage]>(status: status, closed: closedReason(status, json))
    if reply.isSuccess {
      guard let items = json?["messages"] as? [[String: Any]] else {
        reply.status = 0
        return reply
      }
      var messages: [PairingMessage] = []
      for item in items {
        guard let seq = item["seq"] as? Int, let text = item["data"] as? String,
          let data = Data(base64Encoded: text)
        else {
          reply.status = 0
          return reply
        }
        messages.append(PairingMessage(seq: seq, data: data))
      }
      reply.value = messages
    }
    return reply
  }

  public func close(slot: Int, by: PairingRole, reason: PairingClosedReason) async {
    var c = URLComponents(url: Self.pairURL(serverURL, "\(slot)"), resolvingAgainstBaseURL: false)!
    c.queryItems = [URLQueryItem(name: "reason", value: reason.rawValue)]
    _ = await request("DELETE", c.url!, role: by, body: nil)
  }

  private func closedReason(_ status: Int, _ json: [String: Any]?) -> PairingClosedReason? {
    guard status == 410, let reason = json?["reason"] as? String else { return nil }
    return PairingClosedReason(rawValue: reason)
  }

  /// The status (0 on a network error) and the JSON object reply, if any.
  private func request(
    _ method: String, _ url: URL, role: PairingRole?, body: [String: String]?,
    timeout: TimeInterval = 30
  ) async -> (Int, [String: Any]?) {
    var request = URLRequest(url: url, timeoutInterval: timeout)
    request.httpMethod = method
    if let body {
      request.httpBody = try? JSONSerialization.data(withJSONObject: body)
      request.setValue("application/json", forHTTPHeaderField: "Content-Type")
    }
    if role == .a, let authKey {
      request.setValue(
        "Bearer " + Chain.accessToken(authKey: authKey, now: Date()),
        forHTTPHeaderField: "Authorization")
    } else if role == .b, let token = lock.withLock({ bToken }) {
      request.setValue(token, forHTTPHeaderField: "X-Compass-Pair-Token")
    }
    guard let (data, response) = try? await session.data(for: request),
      let http = response as? HTTPURLResponse
    else { return (0, nil) }
    let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
    return (http.statusCode, json)
  }
}
