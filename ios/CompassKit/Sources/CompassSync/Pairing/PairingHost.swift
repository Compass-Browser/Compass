// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation

/// A's side of pairing: a device already syncing adds another one. A port
/// of the desktop's PairingHost (components/sync/pairing.cc), with the same
/// messages, results and slot closes, as one async function:
///
/// 1. Opens a slot (signed) and shows `<slot>-<6 digits>` (`onCode`).
/// 2. Waits for B's SPAKE2 message, answers with its own.
/// 3. Checks B's confirmation MAC (a wrong one closes the slot with
///    `mismatch`: a wrong code) and opens B's device name (`onRequest`).
/// 4. After `answer(allow:)`: MAC_a || verdict [|| sealed seed and the
///    chain's profiles]. A deny closes the slot with `deny`; an allow leaves
///    it for B to read and close.
///
/// The slot's life (the open's `expires_in`) bounds everything until the
/// answer is on its way, as the desktop's expiry timer. Cancelling the task
/// running `run` closes the slot with `cancel`.
public final class PairingHost: @unchecked Sendable {
  public enum Result: Sendable, Equatable {
    case added
    case denied
    /// B's confirmation didn't match: a wrong code, or someone in the middle.
    case wrongCode
    /// B gave up (or the host was canceled).
    case canceled
    case expired
    case rateLimited
    case failed
  }

  public static let retryDelay: TimeInterval = 2

  let transport: any PairingTransport
  let host: String
  let seed: Data
  let profiles: [ChainProfile]
  let retryDelay: TimeInterval

  private enum Phase { case waiting, asking, answering, expired }
  private let lock = NSLock()
  private var phase = Phase.waiting
  private var answerContinuation: CheckedContinuation<Bool?, Never>?
  private var pendingAnswer: Bool?
  private var askCanceled = false

  /// `host`: the server's host name (lowercase, no port), bound into SPAKE2
  /// unless the server names itself. `profiles`: the chain's list, sent to
  /// B with the seed (empty = unknown).
  public init(
    transport: any PairingTransport, host: String, seed: Data, profiles: [ChainProfile] = [],
    retryDelay: TimeInterval = PairingHost.retryDelay
  ) {
    self.transport = transport
    self.host = host
    self.seed = seed
    self.profiles = profiles
    self.retryDelay = retryDelay
  }

  /// The user's answer to `onRequest`'s "Add <device>?". Ignored at any
  /// other time.
  public func answer(allow: Bool) {
    let continuation: CheckedContinuation<Bool?, Never>? = lock.withLock {
      guard phase == .asking else { return nil }
      if let c = answerContinuation {
        answerContinuation = nil
        return c
      }
      pendingAnswer = allow
      return nil
    }
    continuation?.resume(returning: allow)
  }

  /// Runs one pairing. `onCode` gets the code to show and how long it's
  /// valid; `onRequest` the name of the device that proved it knows it.
  public func run(
    onCode: @escaping @Sendable (String, TimeInterval) async -> Void,
    onRequest: @escaping @Sendable (String) async -> Void
  ) async -> Result {
    let opened = await transport.open()
    guard opened.status == 201, let info = opened.value, info.slot >= 1 else {
      // 429: too many opens from this address; 503: the server's slots are
      // full. Both mean "try again later".
      return opened.status == 429 || opened.status == 503 ? .rateLimited : .failed
    }
    // The server's own name, so both sides bind the same one however each
    // reaches it (an IP, a Tailscale name).
    let host = info.server.isEmpty ? self.host : info.server
    let code = PairingCode(slot: info.slot, digits: PairingCode.randomDigits())
    // Our SPAKE2 message, before the code shows (as the desktop).
    guard let spake = Spake2(role: .a, host: host, slot: code.slot),
      let alice = spake.generateMessage(password: code.digits)
    else {
      await close(slot: code.slot, reason: .cancel)
      return .failed
    }
    let deadline = Date().addingTimeInterval(info.expiresIn)
    await onCode(code.text, info.expiresIn)
    let box = SpakeBox(spake)

    let (result, closeReason) = await withTaskGroup(of: (Result, PairingClosedReason?)?.self) { group in
      group.addTask {
        await self.exchange(slot: code.slot, spake: box.spake, alice: alice, onRequest: onRequest)
      }
      group.addTask { await self.expire(at: deadline) }
      var first: (Result, PairingClosedReason?)?
      for await outcome in group {
        if let outcome, first == nil {
          first = outcome
          group.cancelAll()
        }
      }
      return first ?? (.canceled, .cancel)
    }
    if let closeReason { await close(slot: code.slot, reason: closeReason) }
    return result
  }

  /// The slot's end: `.expired` (closing it) unless the answer is already
  /// on its way; nil when canceled or answering.
  private func expire(at deadline: Date) async -> (Result, PairingClosedReason?)? {
    let wait = deadline.timeIntervalSinceNow
    if wait > 0 {
      do { try await Task.sleep(for: .seconds(wait)) } catch { return nil }
    }
    let continuation: CheckedContinuation<Bool?, Never>?
    let expired: Bool
    (expired, continuation) = lock.withLock {
      guard phase != .answering else { return (false, nil) }
      phase = .expired
      let c = answerContinuation
      answerContinuation = nil
      return (true, c)
    }
    continuation?.resume(returning: nil)
    return expired ? (.expired, .cancel) : nil
  }

  /// Steps 1 to 4. The result, and the reason to close the slot with (nil
  /// when the server already closed it, or B is to close it).
  private func exchange(
    slot: Int, spake: Spake2, alice: Data, onRequest: @Sendable (String) async -> Void
  ) async -> (Result, PairingClosedReason?) {
    // 1. B's SPAKE2 message.
    var lastSeq = 0
    let bob: PairingMessage
    switch await nextMessage(slot: slot, after: &lastSeq) {
    case .failure(let end): return end.value
    case .success(let message): bob = message
    }
    guard let keys = spake.processMessage(bob.data) else { return (.wrongCode, .mismatch) }

    // 2. Ours.
    let posted = await transport.send(slot: slot, from: .a, data: alice)
    if posted.status == 410 { return (Self.result(closed: posted.closed), nil) }
    if posted.status == 404 { return (.expired, nil) }
    if !posted.isSuccess { return (posted.status == 429 ? .rateLimited : .failed, .cancel) }

    // 3. MAC_b || sealed device name.
    let confirmation: PairingMessage
    switch await nextMessage(slot: slot, after: &lastSeq) {
    case .failure(let end): return end.value
    case .success(let message): confirmation = message
    }
    let data = confirmation.data
    guard data.count >= 32, keys.verifyMac(Data(data.prefix(32)), sender: .b) else {
      return (.wrongCode, .mismatch)
    }
    guard let name = keys.open(Data(data.dropFirst(32)), from: .b) else { return (.failed, .cancel) }

    guard let allow = await ask(String(decoding: name, as: UTF8.self), onRequest: onRequest) else {
      return (Task.isCancelled ? .canceled : .expired, .cancel)
    }

    // 4. MAC_a || verdict [|| sealed seed].
    var answer = keys.confirmMac(sender: .a)
    answer.append(allow ? 0x01 : 0x00)
    if allow { answer.append(keys.seal(Self.payload(seed: seed, profiles: profiles), from: .a)) }
    let sent = await transport.send(slot: slot, from: .a, data: answer)
    if sent.status == 410 { return (Self.result(closed: sent.closed), nil) }
    if !sent.isSuccess { return sent.status == 404 ? (.expired, nil) : (.failed, .cancel) }
    // Allowed: B reads the answer, then closes the slot. Denied: B learns
    // it from the answer or, if the slot closes first, from the server.
    return allow ? (.added, nil) : (.denied, .deny)
  }

  /// Asks the user; nil if the slot expired or pairing was canceled first.
  private func ask(_ name: String, onRequest: @Sendable (String) async -> Void) async -> Bool? {
    let ready = lock.withLock {
      guard phase == .waiting else { return false }
      phase = .asking
      return true
    }
    guard ready, !Task.isCancelled else { return nil }
    await onRequest(name)
    let allow: Bool? = await withTaskCancellationHandler {
      await withCheckedContinuation { (c: CheckedContinuation<Bool?, Never>) in
        let early: Bool?? = lock.withLock {
          if phase != .asking || askCanceled { return .some(nil) }
          if let pending = pendingAnswer { return .some(pending) }
          answerContinuation = c
          return nil
        }
        if let early { c.resume(returning: early) }
      }
    } onCancel: {
      let c: CheckedContinuation<Bool?, Never>? = lock.withLock {
        askCanceled = true
        defer { answerContinuation = nil }
        return answerContinuation
      }
      c?.resume(returning: nil)
    }
    guard let allow, !Task.isCancelled else { return nil }
    // Past this point the slot's end no longer interrupts.
    return lock.withLock {
      guard phase == .asking else { return nil }
      phase = .answering
      return allow
    }
  }

  /// The next message from B after `lastSeq`, long-polling.
  private func nextMessage(slot: Int, after lastSeq: inout Int) async -> Swift.Result<PairingMessage, HostEnd> {
    while true {
      if Task.isCancelled { return .failure(HostEnd((.canceled, .cancel))) }
      let reply = await transport.receive(slot: slot, reader: .a, after: lastSeq)
      if Task.isCancelled { return .failure(HostEnd((.canceled, .cancel))) }
      switch reply.status {
      case 0:
        try? await Task.sleep(for: .seconds(retryDelay))
        continue
      case 410: return .failure(HostEnd((Self.result(closed: reply.closed), nil)))
      case 404: return .failure(HostEnd((.expired, nil)))
      default: break
      }
      guard reply.isSuccess else { return .failure(HostEnd((.failed, .cancel))) }
      let messages = (reply.value ?? []).sorted { $0.seq < $1.seq }.filter { $0.seq > lastSeq }
      if let first = messages.first {
        lastSeq = first.seq
        return .success(first)
      }
    }
  }

  /// Closes the slot even from a canceled task (URLSession refuses requests
  /// from one).
  private func close(slot: Int, reason: PairingClosedReason) async {
    let transport = self.transport
    await Task.detached { await transport.close(slot: slot, by: .a, reason: reason) }.value
  }

  /// What A makes of a slot the server says closed.
  static func result(closed: PairingClosedReason?) -> Result {
    switch closed {
    case .mismatch: .wrongCode
    case .cancel: .canceled
    case .deny: .denied
    case .expired, nil: .expired
    }
  }

  /// Message 4's plaintext: {"profiles": [...], "seed": base64}, as the
  /// desktop's base::WriteJson writes it (sorted keys, "/" unescaped).
  static func payload(seed: Data, profiles: [ChainProfile]) -> Data {
    let object: [String: Any] = ["seed": seed.base64EncodedString(), "profiles": ChainProfile.json(profiles)]
    return try! JSONSerialization.data(withJSONObject: object, options: [.sortedKeys, .withoutEscapingSlashes])
  }
}

/// Hands the SPAKE2 context to the exchange's task; only that task uses it.
private struct SpakeBox: @unchecked Sendable {
  let spake: Spake2
  init(_ spake: Spake2) { self.spake = spake }
}

/// A non-generic error box for `Result`'s failure (tuples aren't Errors).
struct HostEnd: Error {
  let value: (PairingHost.Result, PairingClosedReason?)
  init(_ value: (PairingHost.Result, PairingClosedReason?)) { self.value = value }
}
