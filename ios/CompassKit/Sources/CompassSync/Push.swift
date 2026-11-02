// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation

/// APNs push for sent tabs (docs/design/send-tab.md, "With push"): the
/// phone's device token, registered per account with the server, and the
/// one-off download the notification service extension runs.
public enum Push {
  public enum Environment: String, Sendable { case sandbox, production }
  public static let topic = "it.cipi1965.compass"

  /// `<server>/compass/push?client_id=<cache GUID>` (the server URL ends in
  /// /v2, as for /command/).
  static func url(_ server: URL, cacheGUID: String) -> URL {
    var c = URLComponents(url: server, resolvingAgainstBaseURL: false)!
    var path = c.path
    if path.hasSuffix("/") { path.removeLast() }
    c.path = path + "/compass/push"
    c.queryItems = [URLQueryItem(name: "client_id", value: cacheGUID)]
    return c.url!
  }

  /// Registers `token` (APNs device token) for one account; throws on
  /// anything but 204.
  public static func register(
    token: Data, environment: Environment, server: URL, cacheGUID: String, authKey: Data,
    session: URLSession = .shared
  ) async throws {
    var request = URLRequest(url: url(server, cacheGUID: cacheGUID), timeoutInterval: 30)
    request.httpMethod = "POST"
    request.setValue("application/json", forHTTPHeaderField: "Content-Type")
    request.setValue("Bearer " + Chain.accessToken(authKey: authKey, now: Date()), forHTTPHeaderField: "Authorization")
    let hex = token.map { String(format: "%02x", $0) }.joined()
    request.httpBody = try JSONSerialization.data(withJSONObject: [
      "token": hex, "environment": environment.rawValue, "topic": topic,
    ])
    try await send(request, session: session)
  }

  /// Forgets this device's token for one account (leaving the chain).
  public static func unregister(server: URL, cacheGUID: String, authKey: Data, session: URLSession = .shared)
    async throws
  {
    var request = URLRequest(url: url(server, cacheGUID: cacheGUID), timeoutInterval: 30)
    request.httpMethod = "DELETE"
    request.setValue("Bearer " + Chain.accessToken(authKey: authKey, now: Date()), forHTTPHeaderField: "Authorization")
    try await send(request, session: session)
  }

  private static func send(_ request: URLRequest, session: URLSession) async throws {
    let (_, response): (Data, URLResponse)
    do {
      (_, response) = try await session.data(for: request)
    } catch {
      throw SyncConnectionError.network(error.localizedDescription)
    }
    guard let http = response as? HTTPURLResponse else { throw SyncConnectionError.badReply }
    guard http.statusCode == 204 else { throw SyncConnectionError.http(http.statusCode) }
  }

  /// The tabs sent to this device that the server holds (unopened, at most
  /// 10 days old), newest first: a throwaway engine with only
  /// SEND_TAB_TO_SELF, so nothing of the app's sync state is touched (the
  /// notification service extension runs in its own process).
  public static func sentTabs(server: URL, chain: Chain, profile: String, cacheGUID: String) async throws -> [SentTab] {
    let authKey = chain.authKey(profileUUID: profile)
    let engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server, cacheGUID: cacheGUID, authKey: authKey),
      passphrase: chain.passphrase(profileUUID: profile), store: MemorySyncMetadataStore(SyncMetadata(cacheGUID: cacheGUID)),
      createChainIfEmpty: false)
    let bridge = SendTabBridge(engine: engine, cacheGUID: cacheGUID, stateURL: nil)
    let collected = Collected()
    bridge.onReceived = { collected.add($0) }
    await engine.register(bridge)
    try await engine.sync()
    return collected.tabs.sorted { $0.sharedTime > $1.sharedTime }
  }

  private final class Collected: @unchecked Sendable {
    private let lock = NSLock()
    private var list: [SentTab] = []
    func add(_ new: [SentTab]) { lock.withLock { list += new } }
    var tabs: [SentTab] { lock.withLock { list } }
  }
}

/// One account of this phone, as the app lists them for the notification
/// service extension (PushAccounts.json in the App Group).
public struct PushAccount: Codable, Sendable, Equatable {
  /// "" for the home account, else the chain profile's UUID.
  public var profile: String
  public var cacheGUID: String
  public init(profile: String, cacheGUID: String) {
    self.profile = profile
    self.cacheGUID = cacheGUID
  }
}
