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

  /// How a connection ended.
  enum Outcome { case connected, failed, removed }

  /// Runs until cancelled, or until the server says another device removed
  /// this one (then `removed` is called and it stops: the server refuses
  /// this cache GUID for good). `changed` gets the types another device
  /// changed, or nil to refresh everything (on each (re)connect: changes
  /// made while disconnected weren't announced).
  public func run(
    changed: @escaping @Sendable ([DataType]?) async -> Void,
    removed: @escaping @Sendable () async -> Void = {}
  ) async {
    var failures = 0
    while !Task.isCancelled {
      let outcome = await stream(changed: changed)
      if Task.isCancelled { return }
      if outcome == .removed {
        await removed()
        return
      }
      failures = outcome == .connected ? 0 : failures + 1
      // 1 s, 2 s, 4 s … up to a minute.
      let delay = min(60, pow(2, Double(min(failures, 6))))
      try? await Task.sleep(for: .seconds(delay))
    }
  }

  /// One connection: whether it got as far as a 200.
  private func stream(changed: @escaping @Sendable ([DataType]?) async -> Void) async -> Outcome {
    var request = URLRequest(url: url)
    request.setValue("text/event-stream", forHTTPHeaderField: "Accept")
    request.setValue("Bearer " + Chain.accessToken(authKey: authKey, now: Date()), forHTTPHeaderField: "Authorization")
    guard let (bytes, response) = try? await session.bytes(for: request),
      let http = response as? HTTPURLResponse
    else { return .failed }
    guard http.statusCode == 200 else {
      return SyncConnectionError.status(http) == .deviceRemoved ? .removed : .failed
    }
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
