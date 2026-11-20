// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import Testing

/// A device joining (side B), scripted against the relay byte by byte, as
/// the desktop's PairingJoiner sends.
struct ScriptedJoiner {
  enum Outcome: Equatable {
    /// Message 4's verdict and its plaintext (allow only).
    case answered(allow: Bool, payload: Data?)
    /// The slot closed before an answer came.
    case closed(PairingClosedReason?)
    /// A's confirmation didn't verify.
    case badMac
  }

  let relay: FakeRelay
  let name: String

  func run(digits: String) async -> Outcome {
    guard await relay.join(slot: relay.slot).isSuccess else { return .closed(await relay.closed) }
    let spake = Spake2(role: .b, host: relay.server, slot: relay.slot)!
    _ = await relay.send(from: .b, data: spake.generateMessage(password: digits)!)
    guard let alice = await waitFor(1) else { return .closed(await relay.closed) }
    let keys = spake.processMessage(alice)!
    _ = await relay.send(from: .b, data: keys.confirmMac(sender: .b) + keys.seal(Data(name.utf8), from: .b))
    guard let answer = await waitFor(2) else { return .closed(await relay.closed) }
    guard keys.verifyMac(Data(answer.prefix(32)), sender: .a) else { return .badMac }
    let allow = answer[answer.startIndex + 32] == 0x01
    if allow {
      #expect(answer.count > 33 + 12 + 16)
    } else {
      #expect(answer.count == 33)
    }
    return .answered(allow: allow, payload: allow ? keys.open(Data(answer.dropFirst(33)), from: .a) : nil)
  }

  /// Message `seq` from A, or nil once the slot closed without it.
  private func waitFor(_ seq: Int) async -> Data? {
    while !Task.isCancelled {
      let reply = await relay.receive(reader: .b, after: seq - 1)
      if let m = reply.value?.first(where: { $0.seq == seq }) { return m.data }
      if reply.status == 410 { return nil }
      try? await Task.sleep(for: .milliseconds(5))
    }
    return nil
  }
}

/// Collects what the host shows, and answers for the user.
actor HostUI {
  var code: String?
  var expiresIn: TimeInterval?
  var requests: [String] = []
  private var codeWaiters: [CheckedContinuation<String, Never>] = []

  func showCode(_ code: String, _ expiresIn: TimeInterval) {
    self.code = code
    self.expiresIn = expiresIn
    for w in codeWaiters { w.resume(returning: code) }
    codeWaiters = []
  }

  func request(_ name: String) { requests.append(name) }

  func waitForCode() async -> String {
    if let code { return code }
    return await withCheckedContinuation { codeWaiters.append($0) }
  }
}

struct PairingHostTests {
  let seed = Data(hex: "000102030405060708090a0b0c0d0e0f")!
  let profiles = [
    ChainProfile(uuid: "2b5c3e1a-0000-4000-8000-000000000001", name: "Home", avatar: 27, home: true),
    ChainProfile(uuid: "2b5c3e1a-0000-4000-8000-000000000002", name: "Work/Ops"),
  ]

  struct Run {
    let result: PairingHost.Result
    let outcome: ScriptedJoiner.Outcome?
    let ui: HostUI
    let relay: FakeRelay
  }

  /// The host against a scripted joiner typing `digits` (nil: the code's
  /// own); `answer` is the user's (nil: never answers).
  func pair(
    digits: String? = nil, answer: Bool? = true, expiresIn: TimeInterval = 300, joins: Bool = true
  ) async -> Run {
    let relay = FakeRelay(slot: 7, server: "sync.example.com")
    await relay.setOpen(expiresIn: expiresIn)
    let ui = HostUI()
    let host = PairingHost(
      transport: FakeTransport(relay: relay), host: "typed.example.com", seed: seed, profiles: profiles,
      retryDelay: 0.01)
    let joiner = ScriptedJoiner(relay: relay, name: "Pixel 9")
    let joinTask = Task { () -> ScriptedJoiner.Outcome? in
      guard joins else { return nil }
      let code = await ui.waitForCode()
      return await joiner.run(digits: digits ?? PairingCode(code)!.digits)
    }
    let result = await host.run(
      onCode: { code, expires in await ui.showCode(code, expires) },
      onRequest: { name in
        await ui.request(name)
        if let answer { host.answer(allow: answer) }
      })
    let outcome: ScriptedJoiner.Outcome?
    if joins, [.added, .denied, .wrongCode].contains(result) {
      outcome = await joinTask.value
    } else {
      joinTask.cancel()
      outcome = nil
    }
    return Run(result: result, outcome: outcome, ui: ui, relay: relay)
  }

  @Test func rightCodeAdds() async throws {
    let run = await pair()
    #expect(run.result == .added)
    let code = try #require(await run.ui.code)
    #expect(code.hasPrefix("7-") && PairingCode(code) != nil)
    #expect(await run.ui.expiresIn == 300)
    #expect(await run.ui.requests == ["Pixel 9"])
    // The seed and the chain's profiles, as the desktop's base::WriteJson
    // writes them (sorted keys; "avatar" is the index).
    guard case .answered(true, let payload?) = run.outcome else {
      Issue.record("no allow: \(String(describing: run.outcome))")
      return
    }
    #expect(
      String(decoding: payload, as: UTF8.self)
        == #"{"profiles":[{"avatar":26,"home":true,"name":"Home","uuid":"2b5c3e1a-0000-4000-8000-000000000001"},"#
        + #"{"name":"Work/Ops","uuid":"2b5c3e1a-0000-4000-8000-000000000002"}],"seed":"AAECAwQFBgcICQoLDA0ODw=="}"#)
    // A leaves the slot open for B to read the answer.
    #expect(await run.relay.closes.isEmpty)
  }

  @Test func wrongCodeClosesWithMismatch() async {
    let run = await pair(digits: "000000")
    #expect(run.result == .wrongCode)
    #expect(await run.ui.requests.isEmpty)
    let closes = await run.relay.closes
    #expect(closes.count == 1 && closes[0].0 == .a && closes[0].1 == .mismatch)
    #expect(run.outcome == .closed(.mismatch))
  }

  @Test func denyClosesWithDeny() async {
    let run = await pair(answer: false)
    #expect(run.result == .denied)
    #expect(run.outcome == .answered(allow: false, payload: nil))
    let closes = await run.relay.closes
    #expect(closes.count == 1 && closes[0].0 == .a && closes[0].1 == .deny)
  }

  @Test func expiresWithoutAJoiner() async {
    let run = await pair(expiresIn: 0.3, joins: false)
    #expect(run.result == .expired)
    let closes = await run.relay.closes
    #expect(closes.count == 1 && closes[0].0 == .a && closes[0].1 == .cancel)
  }

  @Test func expiresWhileAsking() async {
    let run = await pair(answer: nil, expiresIn: 0.5)
    #expect(run.result == .expired)
    #expect(await run.ui.requests == ["Pixel 9"])
    let closes = await run.relay.closes
    #expect(closes.count == 1 && closes[0].0 == .a && closes[0].1 == .cancel)
  }

  @Test func cancelClosesTheSlot() async throws {
    let relay = FakeRelay(slot: 3, server: "s")
    let ui = HostUI()
    let host = PairingHost(transport: FakeTransport(relay: relay), host: "s", seed: seed, retryDelay: 0.01)
    let task = Task { await host.run(onCode: { await ui.showCode($0, $1) }, onRequest: { _ in }) }
    _ = await ui.waitForCode()
    task.cancel()
    #expect(await task.value == .canceled)
    let closes = await relay.closes
    #expect(closes.count == 1 && closes[0].0 == .a && closes[0].1 == .cancel)
  }

  @Test func cancelWhileAsking() async throws {
    let relay = FakeRelay(slot: 3, server: "s")
    let ui = HostUI()
    let host = PairingHost(transport: FakeTransport(relay: relay), host: "s", seed: seed, retryDelay: 0.01)
    let task = Task { await host.run(onCode: { await ui.showCode($0, $1) }, onRequest: { await ui.request($0) }) }
    let code = await ui.waitForCode()
    let joiner = Task { await ScriptedJoiner(relay: relay, name: "Mac").run(digits: PairingCode(code)!.digits) }
    while await ui.requests.isEmpty { try await Task.sleep(for: .milliseconds(5)) }
    task.cancel()
    #expect(await task.value == .canceled)
    #expect(await joiner.value == .closed(.cancel))
    // A late answer does nothing.
    host.answer(allow: true)
    #expect(await relay.fromA.count == 1)
  }

  @Test func joinerGivesUp() async throws {
    let relay = FakeRelay(slot: 3, server: "s")
    let ui = HostUI()
    let host = PairingHost(transport: FakeTransport(relay: relay), host: "s", seed: seed, retryDelay: 0.01)
    let task = Task { await host.run(onCode: { await ui.showCode($0, $1) }, onRequest: { _ in }) }
    _ = await ui.waitForCode()
    _ = await relay.join(slot: 3)
    await relay.close(by: .b, reason: .cancel)
    #expect(await task.value == .canceled)
    // The server closed it: A doesn't close it again.
    #expect(await relay.closes.count == 1)
  }

  @Test func busyServer() async {
    for (status, expected) in [(429, PairingHost.Result.rateLimited), (503, .rateLimited), (500, .failed)] {
      let relay = FakeRelay(slot: 1, server: "s")
      await relay.setOpen(status: status)
      let host = PairingHost(transport: FakeTransport(relay: relay), host: "s", seed: seed)
      #expect(await host.run(onCode: { _, _ in }, onRequest: { _ in }) == expected)
      #expect(await relay.closes.isEmpty)
    }
  }

  /// The Swift joiner and the Swift host, through the relay.
  @Test func pairsWithTheJoiner() async {
    let relay = FakeRelay(slot: 12, server: "sync.example.com")
    let ui = HostUI()
    let host = PairingHost(
      transport: FakeTransport(relay: relay), host: "a.example.com", seed: seed, profiles: profiles,
      retryDelay: 0.01)
    let hostTask = Task {
      await host.run(onCode: { await ui.showCode($0, $1) }, onRequest: { _ in host.answer(allow: true) })
    }
    let code = await ui.waitForCode()
    let joiner = PairingJoiner(
      transport: FakeTransport(relay: relay), host: "b.example.com", deviceName: "iPad", retryDelay: 0.01)
    let joined = await joiner.join(code: code)
    #expect(joined == .joined(seed: seed, profiles: profiles))
    #expect(await hostTask.value == .added)
    let closes = await relay.closes
    #expect(closes.count == 1 && closes[0].0 == .b && closes[0].1 == .cancel)
  }

  @Test func closedReasonsMapAsOnTheDesktop() {
    #expect(PairingHost.result(closed: .mismatch) == .wrongCode)
    #expect(PairingHost.result(closed: .cancel) == .canceled)
    #expect(PairingHost.result(closed: .deny) == .denied)
    #expect(PairingHost.result(closed: .expired) == .expired)
    #expect(PairingHost.result(closed: nil) == .expired)
  }
}
