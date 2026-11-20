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

  public init(serverURL: URL, cacheGUID: String, authKey: Data) {
    var c = URLComponents(url: serverURL, resolvingAgainstBaseURL: false)!
    var path = c.path
    while path.hasSuffix("/") { path.removeLast() }
    c.path = path + "/compass/events"
    c.queryItems = [URLQueryItem(name: "client_id", value: cacheGUID)]
    url = c.url!
    self.authKey = authKey
    let configuration = URLSessionConfiguration.default
    configuration.timeoutIntervalForRequest = 90  // keep-alives every 25 s
    configuration.timeoutIntervalForResource = 24 * 60 * 60
    session = URLSession(configuration: configuration)
  }

  /// Runs until cancelled. `changed` gets the types another device changed,
  /// or nil to refresh everything (on each (re)connect: changes made while
  /// disconnected weren't announced). `refused` hears each 401 (true when
  /// it's the sign-in gate's, see LoginClient): the owner should stop the
  /// stream until it has signed in, not let it retry every minute.
  public func run(
    changed: @escaping @Sendable ([DataType]?) async -> Void,
    refused: (@Sendable (_ loginRequired: Bool) async -> Void)? = nil
  ) async {
    var failures = 0
    while !Task.isCancelled {
      let outcome = await stream(changed: changed)
      if Task.isCancelled { return }
      if case .refused(let loginRequired) = outcome { await refused?(loginRequired) }
      if Task.isCancelled { return }
      failures = outcome == .connected ? 0 : failures + 1
      // 1 s, 2 s, 4 s … up to a minute.
      let delay = min(60, pow(2, Double(min(failures, 6))))
      try? await Task.sleep(for: .seconds(delay))
    }
  }

  private enum Outcome: Equatable {
    case connected
    case failed
    case refused(loginRequired: Bool)
  }

  /// One connection: a 200 (connected), a 401, or anything else.
  private func stream(changed: @escaping @Sendable ([DataType]?) async -> Void) async -> Outcome {
    var request = URLRequest(url: url)
    request.setValue("text/event-stream", forHTTPHeaderField: "Accept")
    request.setValue("Bearer " + Chain.accessToken(authKey: authKey, now: Date()), forHTTPHeaderField: "Authorization")
    guard let (bytes, response) = try? await session.bytes(for: request), let http = response as? HTTPURLResponse
    else { return .failed }
    if http.statusCode == 401 { return .refused(loginRequired: LoginGate.isLoginRequired(http)) }
    guard http.statusCode == 200 else { return .failed }
    await changed(nil)
    var parser = EventStreamParser()
    do {
      for try await byte in bytes {
        for event in parser.feed(byte) where event.name == "changed" {
          await changed(Self.types(event.data))
        }
      }
    } catch {}
    return .connected
  }

  /// The data types of a `changed` event; nil (refresh all) if unreadable.
  static func types(_ data: String) -> [DataType]? {
    guard let json = try? JSONSerialization.jsonObject(with: Data(data.utf8)) as? [String: Any],
      let numbers = json["types"] as? [Int]
    else { return nil }
    return numbers.compactMap { DataType(rawValue: Int32($0)) }
  }
}
