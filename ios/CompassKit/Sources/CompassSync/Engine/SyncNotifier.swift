// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassShared
import Foundation

/// Change notifications from the server (sync.md, "Change notifications"): a
/// Server-Sent Events stream at `<server>/compass/events?client_id=<cache
/// GUID>`, signed like /command/. After another device commits, it sends
///
///     event: changed
///     data: {"types":[32904,970000]}
///
/// (EntitySpecifics field numbers). The phone holds it while the app is in
/// the foreground; iOS doesn't allow a stream in the background.
public final class SyncNotifier: Sendable {
  let url: URL
  let authKey: Data
  let session: URLSession

  /// `protocolClasses`: tests' stubs.
  public init(serverURL: URL, cacheGUID: String, authKey: Data, protocolClasses: [AnyClass]? = nil) {
    url = ServerEndpoint.url(serverURL, path: "compass/events", query: ["client_id": cacheGUID])
    self.authKey = authKey
    let configuration = URLSessionConfiguration.default
    configuration.timeoutIntervalForRequest = 90  // keep-alives every 25 s
    configuration.timeoutIntervalForResource = 24 * 60 * 60
    if let protocolClasses { configuration.protocolClasses = protocolClasses }
    session = URLSession(configuration: configuration)
  }

  /// How a connection ended: a 200, the server's word that another device
  /// removed this one, a 401 (true when it's the sign-in gate's, see
  /// LoginClient), or anything else.
  enum Outcome: Equatable {
    case connected
    case failed
    case removed
    case refused(loginRequired: Bool)
  }

  /// A room's change (docs/design/sync.md, "Rooms", Notifications):
  ///
  ///     event: room
  ///     data: {"room_id":"n4bQgYhMfWWaL-qgxVrQFQ","seq":42,"changes":["records"]}
  public struct RoomEvent: Sendable, Equatable {
    public var roomID: String
    public var seq: Int64
    /// "records", "members", "keys", "invites", "deleted".
    public var changes: [String]

    public init(roomID: String, seq: Int64, changes: [String]) {
      self.roomID = roomID
      self.seq = seq
      self.changes = changes
    }
  }

  /// Runs until cancelled, or until the server says another device removed
  /// this one (then `removed` is called and it stops: the server refuses
  /// this cache GUID for good). `changed` gets the types another device
  /// changed, or nil to refresh everything (on each (re)connect: changes
  /// made while disconnected weren't announced). `refused` hears each 401
  /// (true when it's the sign-in gate's): the owner should stop the stream
  /// until it has signed in, not let it retry every minute. `room` gets the
  /// `room` events of the rooms the account is a member of (shared spaces,
  /// folders and easels); nothing else changes for those who don't pass it.
  public func run(
    changed: @escaping @Sendable ([DataType]?) async -> Void,
    removed: @escaping @Sendable () async -> Void = {},
    refused: (@Sendable (_ loginRequired: Bool) async -> Void)? = nil,
    room: (@Sendable (RoomEvent) async -> Void)? = nil
  ) async {
    var failures = 0
    while !Task.isCancelled {
      let outcome = await stream(changed: changed, room: room)
      if Task.isCancelled { return }
      if outcome == .removed {
        await removed()
        return
      }
      if case .refused(let loginRequired) = outcome { await refused?(loginRequired) }
      if Task.isCancelled { return }
      failures = outcome == .connected ? 0 : failures + 1
      // 1 s, 2 s, 4 s … up to a minute.
      let delay = min(60, pow(2, Double(min(failures, 6))))
      try? await Task.sleep(for: .seconds(delay))
    }
  }

  /// One connection: how it ended.
  private func stream(
    changed: @escaping @Sendable ([DataType]?) async -> Void, room: (@Sendable (RoomEvent) async -> Void)?
  ) async -> Outcome {
    var request = URLRequest(url: url)
    request.setValue("text/event-stream", forHTTPHeaderField: "Accept")
    request.setValue("Bearer " + Chain.accessToken(authKey: authKey, now: Date()), forHTTPHeaderField: "Authorization")
    guard let (bytes, response) = try? await session.bytes(for: request),
      let http = response as? HTTPURLResponse
    else { return .failed }
    if http.statusCode == 401 { return .refused(loginRequired: LoginGate.isLoginRequired(http)) }
    guard http.statusCode == 200 else {
      return SyncConnectionError.status(http) == .deviceRemoved ? .removed : .failed
    }
    await changed(nil)
    var parser = EventStreamParser()
    do {
      for try await byte in bytes {
        for event in parser.feed(byte) {
          if event.name == "changed" {
            await changed(Self.types(event.data))
          } else if event.name == "room", let room, let roomEvent = Self.roomEvent(event.data) {
            await room(roomEvent)
          }
        }
      }
    } catch {}
    return .connected
  }

  /// A `room` event's data; nil if unreadable.
  static func roomEvent(_ data: String) -> RoomEvent? {
    guard let json = try? JSONSerialization.jsonObject(with: Data(data.utf8)) as? [String: Any],
      let id = json["room_id"] as? String, !id.isEmpty
    else { return nil }
    return RoomEvent(
      roomID: id, seq: (json["seq"] as? NSNumber)?.int64Value ?? 0, changes: json["changes"] as? [String] ?? [])
  }

  /// The data types of a `changed` event; nil (refresh all) if unreadable.
  static func types(_ data: String) -> [DataType]? {
    guard let json = try? JSONSerialization.jsonObject(with: Data(data.utf8)) as? [String: Any],
      let numbers = json["types"] as? [Int]
    else { return nil }
    return numbers.compactMap { DataType(rawValue: Int32($0)) }
  }
}
