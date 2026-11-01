// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import Testing

/// The server's pairing relay, in memory: one slot, messages per sender.
actor FakeRelay {
  var joined = false
  var closed: PairingClosedReason?
  var fromA: [PairingMessage] = []
  var fromB: [PairingMessage] = []
  var closes: [(PairingRole, PairingClosedReason)] = []
  let server: String
  let slot: Int

  init(slot: Int, server: String) {
    self.slot = slot
    self.server = server
  }

  func join(slot: Int) -> PairingReply<PairingJoinInfo> {
    guard slot == self.slot else { return .init(status: 404) }
    if let closed { return .init(status: 410, closed: closed) }
    if joined { return .init(status: 409) }
    joined = true
    return .init(status: 200, value: PairingJoinInfo(server: server, expiresIn: 300))
  }

  func send(from: PairingRole, data: Data) -> PairingReply<Int> {
    if let closed { return .init(status: 410, closed: closed) }
    if from == .a {
      fromA.append(PairingMessage(seq: fromA.count + 1, data: data))
      return .init(status: 200, value: fromA.count)
    }
    fromB.append(PairingMessage(seq: fromB.count + 1, data: data))
    return .init(status: 200, value: fromB.count)
  }

  /// Unlike the server, answers at once (an empty list = a long-poll timeout).
  func receive(reader: PairingRole, after: Int) -> PairingReply<[PairingMessage]> {
    let box = reader == .b ? fromA : fromB
    let messages = box.filter { $0.seq > after }
    if messages.isEmpty, let closed { return .init(status: 410, closed: closed) }
    return .init(status: 200, value: messages)
  }

  func close(by: PairingRole, reason: PairingClosedReason) {
    closes.append((by, reason))
    if closed == nil { closed = reason }
  }
}

struct FakeTransport: PairingTransport {
  let relay: FakeRelay
  func open() async -> PairingReply<PairingOpenInfo> { .init(status: 500) }
  func join(slot: Int) async -> PairingReply<PairingJoinInfo> { await relay.join(slot: slot) }
  func send(slot: Int, from: PairingRole, data: Data) async -> PairingReply<Int> {
    await relay.send(from: from, data: data)
  }
  func receive(slot: Int, reader: PairingRole, after: Int) async -> PairingReply<[PairingMessage]> {
    let reply = await relay.receive(reader: reader, after: after)
    if reply.value?.isEmpty == true { try? await Task.sleep(for: .milliseconds(5)) }
    return reply
  }
  func close(slot: Int, by: PairingRole, reason: PairingClosedReason) async {
    await relay.close(by: by, reason: reason)
  }
}

/// A Mac (side A) scripted against the relay, as the desktop's PairingHost.
struct ScriptedMac {
  let relay: FakeRelay
  let digits: String
  let seed: Data
  let allow: Bool

  /// The device name B sent, or nil when B's confirmation didn't verify.
  func run() async -> String? {
    let spake = Spake2(role: .a, host: relay.server, slot: relay.slot)!
    let bob = await waitFor(1)
    _ = await relay.send(from: .a, data: spake.generateMessage(password: digits)!)
    guard let keys = spake.processMessage(bob) else { return nil }
    let confirmation = await waitFor(2)
    guard keys.verifyMac(confirmation.prefix(32), sender: .b) else {
      await relay.close(by: .a, reason: .mismatch)
      return nil
    }
    let name = keys.open(Data(confirmation.dropFirst(32)), from: .b).map {
      String(decoding: $0, as: UTF8.self)
    }
    var answer = keys.confirmMac(sender: .a)
    if allow {
      answer.append(0x01)
      let json = try! JSONSerialization.data(
        withJSONObject: [
          "seed": seed.base64EncodedString(),
          "profiles": [
            ["uuid": "2b5c3e1a-0000-4000-8000-000000000001", "name": "Home", "home": true],
            ["uuid": "2b5c3e1a-0000-4000-8000-000000000002", "name": "Work", "avatar": 27],
            ["uuid": "NOT-A-UUID", "name": "Bad"],
          ],
        ])
      answer.append(keys.seal(json, from: .a))
    } else {
      answer.append(0x00)
    }
    _ = await relay.send(from: .a, data: answer)
    return name
  }

  private func waitFor(_ seq: Int) async -> Data {
    while !Task.isCancelled {
      if let m = await relay.receive(reader: .a, after: seq - 1).value?.first(where: { $0.seq == seq }) {
        return m.data
      }
      try? await Task.sleep(for: .milliseconds(5))
    }
    return Data(repeating: 0, count: 32)
  }
}

struct PairingJoinerTests {
  let seed = Data(hex: "000102030405060708090a0b0c0d0e0f")!

  func pair(code: String, macDigits: String = "482913", allow: Bool = true)
    async -> (PairingJoiner.Result, String?, FakeRelay)
  {
    let relay = FakeRelay(slot: 7, server: "sync.example.com")
    let mac = ScriptedMac(relay: relay, digits: macDigits, seed: seed, allow: allow)
    let macTask = Task { await mac.run() }
    let joiner = PairingJoiner(
      transport: FakeTransport(relay: relay), host: "typed.example.com", deviceName: "iPhone",
      retryDelay: 0.01)
    let result = await joiner.join(code: code)
    // The Mac only finishes when B got as far as its answer.
    var name: String?
    switch result {
    case .joined, .denied: name = await macTask.value
    default: macTask.cancel()
    }
    return (result, name, relay)
  }

  @Test func rightCodeJoins() async {
    let (result, name, relay) = await pair(code: "7-482913")
    #expect(
      result
        == .joined(
          seed: seed,
          profiles: [
            ChainProfile(uuid: "2b5c3e1a-0000-4000-8000-000000000001", name: "Home", home: true),
            ChainProfile(uuid: "2b5c3e1a-0000-4000-8000-000000000002", name: "Work", avatar: 27),
          ]))
    #expect(name == "iPhone")
    // B closes the slot once it has the answer.
    let closes = await relay.closes
    #expect(closes.count == 1 && closes[0].0 == .b && closes[0].1 == .cancel)
  }

  @Test func denied() async {
    let (result, _, _) = await pair(code: "7-482913", allow: false)
    #expect(result == .denied)
  }

  @Test func wrongCodeIsAMismatch() async {
    let (result, _, relay) = await pair(code: "7-111111")
    // The Mac sees B's MAC fail and closes with "mismatch"; B learns it
    // from the 410.
    #expect(result == .wrongCode)
    #expect(await relay.closed == .mismatch)
  }

  @Test func badCodeNeverJoins() async {
    let (result, _, relay) = await pair(code: "7482913")
    #expect(result == .badCode)
    #expect(await relay.joined == false)
  }

  @Test func unknownSlot() async {
    let (result, _, _) = await pair(code: "8-482913")
    #expect(result == .noSuchCode)
  }

  @Test func usedCode() async {
    let relay = FakeRelay(slot: 7, server: "s")
    _ = await relay.join(slot: 7)
    let joiner = PairingJoiner(transport: FakeTransport(relay: relay), host: "s", deviceName: "x")
    #expect(await joiner.join(code: "7-482913") == .codeUsed)
  }

  @Test func closedReasonsMapAsOnTheDesktop() {
    #expect(PairingJoiner.result(closed: .mismatch) == .wrongCode)
    #expect(PairingJoiner.result(closed: .cancel) == .canceled)
    #expect(PairingJoiner.result(closed: .deny) == .denied)
    #expect(PairingJoiner.result(closed: .expired) == .expired)
    #expect(PairingJoiner.result(closed: nil) == .closed)
  }

  @Test func pairURL() {
    #expect(NetworkPairingTransport.pairURL(URL(string: "https://h/v2/")!, "7/msg").absoluteString
      == "https://h/v2/compass/pair/7/msg")
    #expect(NetworkPairingTransport.pairURL(URL(string: "http://h:8295/v2?x=1")!, "").absoluteString
      == "http://h:8295/v2/compass/pair")
  }
}
