// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation

/// B's side of pairing: the phone joining a chain from a code shown on a
/// Mac. A port of the desktop's PairingJoiner (components/sync/pairing.cc),
/// as one async function instead of callbacks.
public struct PairingJoiner: Sendable {
  public enum Result: Sendable, Equatable {
    case joined(seed: Data)
    case denied
    /// Not "7-482913".
    case badCode
    /// No open slot with that number (or it expired): 404 on join.
    case noSuchCode
    /// Someone already joined that slot: 409.
    case codeUsed
    /// A's confirmation didn't match, or A saw ours didn't.
    case wrongCode
    /// A gave up, or opened a new code.
    case canceled
    /// A closed the slot and the server doesn't say why (an older server).
    case closed
    case expired
    case rateLimited
    case failed
  }

  /// How long B waits when the server doesn't say how long the slot lives.
  public static let defaultTimeout: TimeInterval = 5 * 60 + 30
  /// Past the slot's remaining life, how long B waits.
  public static let timeoutMargin: TimeInterval = 30
  public static let retryDelay: TimeInterval = 2

  let transport: any PairingTransport
  let host: String
  let deviceName: String
  let retryDelay: TimeInterval

  /// `host`: the server's host name (lowercase, no port), bound into SPAKE2
  /// unless the server names itself.
  public init(
    transport: any PairingTransport, host: String, deviceName: String,
    retryDelay: TimeInterval = PairingJoiner.retryDelay
  ) {
    self.transport = transport
    self.host = host
    self.deviceName = deviceName
    self.retryDelay = retryDelay
  }

  /// `waiting` runs once B has proved the code and waits for A's answer
  /// (the Mac asks "Add <device>?").
  public func join(code text: String, waiting: @Sendable () -> Void = {}) async -> Result {
    guard let code = PairingCode(text) else { return .badCode }

    let joined = await transport.join(slot: code.slot)
    guard joined.isSuccess, let info = joined.value else {
      switch joined.status {
      case 410: return Self.result(closed: joined.closed)
      case 404: return .noSuchCode
      case 409: return .codeUsed
      case 429: return .rateLimited
      default: return .failed
      }
    }
    let deadline = Date().addingTimeInterval(
      info.expiresIn > 0 ? info.expiresIn + Self.timeoutMargin : Self.defaultTimeout)
    let host = info.server.isEmpty ? self.host : info.server

    // From here on the slot is ours; every end closes it unless the
    // server already did.
    let (result, close) = await exchange(code: code, host: host, deadline: deadline, waiting: waiting)
    if close {
      await transport.close(
        slot: code.slot, by: .b, reason: result == .wrongCode ? .mismatch : .cancel)
    }
    return result
  }

  /// The result, and whether we still need to close the slot.
  private func exchange(
    code: PairingCode, host: String, deadline: Date, waiting: () -> Void
  ) async -> (Result, Bool) {
    guard let spake = Spake2(role: .b, host: host, slot: code.slot),
      let bob = spake.generateMessage(password: code.digits)
    else { return (.failed, true) }

    // 1. Our SPAKE2 message.
    if let end = Self.end(after: await transport.send(slot: code.slot, from: .b, data: bob)) {
      return end
    }

    // 2. A's SPAKE2 message.
    var lastSeq = 0
    let alice: PairingMessage
    switch await nextMessage(slot: code.slot, after: &lastSeq, deadline: deadline) {
    case .failure(let end): return end.value
    case .success(let message): alice = message
    }
    guard let keys = spake.processMessage(alice.data) else { return (.failed, true) }

    // 3. MAC_b || sealed device name.
    let confirmation = keys.confirmMac(sender: .b) + keys.seal(Data(deviceName.utf8), from: .b)
    if let end = Self.end(
      after: await transport.send(slot: code.slot, from: .b, data: confirmation))
    {
      return end
    }
    waiting()

    // 4. MAC_a || verdict [|| sealed seed].
    let answer: PairingMessage
    switch await nextMessage(slot: code.slot, after: &lastSeq, deadline: deadline) {
    case .failure(let end): return end.value
    case .success(let message): answer = message
    }
    let data = answer.data
    guard data.count >= 33, keys.verifyMac(data.prefix(32), sender: .a) else {
      return (.wrongCode, true)
    }
    guard data[data.startIndex + 32] == 0x01 else { return (.denied, true) }
    guard let json = keys.open(Data(data.dropFirst(33)), from: .a),
      let payload = (try? JSONSerialization.jsonObject(with: json)) as? [String: Any],
      let seedText = payload["seed"] as? String, let seed = Data(base64Encoded: seedText),
      seed.count == Chain.seedLength
    else { return (.failed, true) }
    // A leaves the slot open for us to read the answer; we close it.
    return (.joined(seed: seed), true)
  }

  /// The next message from A after `lastSeq`, long-polling until the deadline.
  private func nextMessage(
    slot: Int, after lastSeq: inout Int, deadline: Date
  ) async -> Swift.Result<PairingMessage, EndBox> {
    while true {
      if Task.isCancelled { return .failure(EndBox((.canceled, true))) }
      if Date() >= deadline { return .failure(EndBox((.expired, true))) }
      let reply = await transport.receive(slot: slot, reader: .b, after: lastSeq)
      switch reply.status {
      case 0:
        try? await Task.sleep(for: .seconds(retryDelay))
        continue
      case 410: return .failure(EndBox((Self.result(closed: reply.closed), false)))
      case 404: return .failure(EndBox((.closed, false)))
      default: break
      }
      guard reply.isSuccess else { return .failure(EndBox((.failed, true))) }
      let messages = (reply.value ?? []).sorted { $0.seq < $1.seq }.filter { $0.seq > lastSeq }
      if let first = messages.first {
        lastSeq = first.seq
        return .success(first)
      }
    }
  }

  /// nil to go on, or how pairing ends after a failed send.
  private static func end(after reply: PairingReply<Int>) -> (Result, Bool)? {
    if reply.status == 410 { return (result(closed: reply.closed), false) }
    if !reply.isSuccess { return reply.status == 404 ? (.closed, false) : (.failed, true) }
    return nil
  }

  static func result(closed: PairingClosedReason?) -> Result {
    switch closed {
    case .mismatch: .wrongCode
    case .cancel: .canceled
    case .deny: .denied
    case .expired: .expired
    case nil: .closed
    }
  }
}

/// A non-generic error box for `Result`'s failure (tuples aren't Errors).
struct EndBox: Error {
  let value: (PairingJoiner.Result, Bool)
  init(_ value: (PairingJoiner.Result, Bool)) { self.value = value }
}
