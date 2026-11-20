// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import Testing

/// The Swift host (side A) pairs the Swift joiner (side B) through the real
/// Go server's pairing endpoints (server/pairing).
@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
struct PairingServerTests {
  let chain = try! Chain(seed: Chain.generateSeed())
  let profiles = [
    ChainProfile(uuid: "2b5c3e1a-0000-4000-8000-000000000001", name: "Home", home: true),
    ChainProfile(uuid: "2b5c3e1a-0000-4000-8000-000000000002", name: "Work", avatar: 3),
  ]

  struct Outcome {
    let host: PairingHost.Result
    let joiner: PairingJoiner.Result
    let requests: [String]
  }

  /// `typed`: what the joiner types, from the code shown.
  func pair(
    server: TestServer, allow: Bool = true, typed: @escaping @Sendable (String) -> String = { $0 }
  ) async -> Outcome {
    let ui = HostUI()
    let host = PairingHost(
      transport: NetworkPairingTransport(serverURL: server.url, authKey: chain.authKey),
      host: server.url.host()!, seed: chain.seed, profiles: profiles)
    let hostTask = Task {
      await host.run(
        onCode: { await ui.showCode($0, $1) },
        onRequest: { name in
          await ui.request(name)
          host.answer(allow: allow)
        })
    }
    let code = await ui.waitForCode()
    let joiner = PairingJoiner(
      transport: NetworkPairingTransport(serverURL: server.url), host: server.url.host()!, deviceName: "iPhone")
    let joined = await joiner.join(code: typed(code))
    return Outcome(host: await hostTask.value, joiner: joined, requests: await ui.requests)
  }

  @Test func addsADevice() async throws {
    let server = try await TestServer()
    let outcome = await pair(server: server)
    #expect(outcome.host == .added, "\(server.log)")
    #expect(outcome.joiner == .joined(seed: chain.seed, profiles: profiles))
    #expect(outcome.requests == ["iPhone"])
  }

  @Test func wrongCode() async throws {
    let server = try await TestServer()
    let outcome = await pair(server: server) { code in
      let c = PairingCode(code)!
      return PairingCode(slot: c.slot, digits: c.digits == "000000" ? "000001" : "000000").text
    }
    #expect(outcome.host == .wrongCode, "\(server.log)")
    #expect(outcome.joiner == .wrongCode)
    #expect(outcome.requests.isEmpty)
  }

  @Test func deny() async throws {
    let server = try await TestServer()
    let outcome = await pair(server: server, allow: false)
    #expect(outcome.host == .denied, "\(server.log)")
    #expect(outcome.joiner == .denied)
  }

  /// Canceling A closes the slot: B, joining late, learns why.
  @Test func cancel() async throws {
    let server = try await TestServer()
    let ui = HostUI()
    let host = PairingHost(
      transport: NetworkPairingTransport(serverURL: server.url, authKey: chain.authKey),
      host: server.url.host()!, seed: chain.seed)
    let hostTask = Task { await host.run(onCode: { await ui.showCode($0, $1) }, onRequest: { _ in }) }
    let code = await ui.waitForCode()
    hostTask.cancel()
    #expect(await hostTask.value == .canceled)
    let joiner = PairingJoiner(
      transport: NetworkPairingTransport(serverURL: server.url), host: server.url.host()!, deviceName: "iPhone")
    #expect(await joiner.join(code: code) == .canceled, "\(server.log)")
  }

  /// Opening needs the chain's signed token.
  @Test func openNeedsTheChainsToken() async throws {
    let server = try await TestServer()
    let reply = await NetworkPairingTransport(serverURL: server.url).open()
    #expect(reply.status == 401)
    let signed = await NetworkPairingTransport(serverURL: server.url, authKey: chain.authKey).open()
    #expect(signed.status == 201 && signed.value?.slot == 1 && signed.value?.expiresIn == 300)
  }
}
