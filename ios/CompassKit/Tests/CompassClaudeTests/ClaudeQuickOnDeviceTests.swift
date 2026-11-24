// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassOnDevice
import Foundation
import Testing

@testable import CompassClaude

/// An on-device model that answers from a script and keeps what it got.
final class FakeOnDevice: @unchecked Sendable {
  private let lock = NSLock()
  private var _asked: [(feature: String, system: String, user: String)] = []
  private var _availability: OnDeviceAvailability = .available
  private var _reply: Result<String, OnDeviceFailure> = .success(#"{"name": "Viaggi"}"#)
  private var _delay: Duration = .zero

  var asked: [(feature: String, system: String, user: String)] { lock.withLock { _asked } }
  func set(availability: OnDeviceAvailability) { lock.withLock { _availability = availability } }
  func set(reply: Result<String, OnDeviceFailure>) { lock.withLock { _reply = reply } }
  func set(delay: Duration) { lock.withLock { _delay = delay } }

  var backend: OnDeviceBackend {
    OnDeviceBackend(
      availability: { [self] in lock.withLock { _availability } },
      answer: { [self] feature, system, user throws(OnDeviceFailure) in
        let (reply, delay) = lock.withLock {
          _asked.append((feature, system, user))
          return (_reply, _delay)
        }
        if delay > .zero { try? await Task.sleep(for: delay) }
        return try reply.get()
      })
  }
}

/// "Answer With" Apple Intelligence (docs/design/ai-sidebar.md, "On this
/// device"), as the desktop's ClaudeQuickOnDeviceTest.
@MainActor
struct ClaudeQuickOnDeviceTests {
  let fake = FakeClaude()
  let device = FakeOnDevice()
  var onDevice = true
  var on: Set<String> = [ClaudeQuick.nameLinkFoldersKey, ClaudeQuick.commandBarAskKey]

  func quick(key: String? = nil) -> ClaudeQuick {
    let (fake, onDevice, on) = (fake, onDevice, on)
    return ClaudeQuick(
      endpoint: { key.map { ClaudeEndpoint(apiKey: $0) } }, isOn: { on.contains($0) },
      transport: { request in try await fake.handle(request) }, answersOnDevice: { onDevice },
      onDevice: device.backend)
  }

  func request() -> ClaudeQuick.Request {
    ClaudeQuick.Request(feature: ClaudeQuick.nameLinkFoldersKey, system: "Claude's prompt.", user: "- A (a.example)")
      .withOnDevicePrompt(
        "folder_name", prompts: "## folder_name\nClaude's.\n## folder_name_on_device\nIn {language}.\n", locale: "it-IT")
  }

  @Test func answersWithoutAKey() async throws {
    let quick = quick()
    #expect(!quick.isSetUp)
    #expect(quick.usesOnDevice(ClaudeQuick.nameLinkFoldersKey))
    #expect(quick.isEnabled(ClaudeQuick.nameLinkFoldersKey))
    // Ask in the command bar stays with the chat's provider.
    #expect(!quick.usesOnDevice(ClaudeQuick.commandBarAskKey))
    #expect(!quick.isEnabled(ClaudeQuick.commandBarAskKey))

    let result = await quick.ask(request())
    #expect(try result.get()["name"] == "Viaggi")
    #expect(device.asked.count == 1)
    #expect(device.asked.first?.feature == "folder_name")
    #expect(device.asked.first?.system == "In Italian.")
    #expect(await fake.count == 0)
  }

  @Test func errors() async {
    let quick = quick()
    device.set(reply: .failure(.refused))
    #expect(await quick.ask(request()) == .failure(.refused))
    device.set(reply: .success("not json"))
    #expect(await quick.ask(request()) == .failure(.badReply))
    device.set(reply: .failure(.unavailable))
    #expect(await quick.ask(request()) == .failure(.notSetUp))
    // Not there (still downloading): the switch can't be on, nothing asked.
    device.set(availability: .notReady)
    #expect(!quick.isEnabled(ClaudeQuick.nameLinkFoldersKey))
    let before = device.asked.count
    #expect(await quick.ask(request()) == .failure(.notSetUp))
    #expect(device.asked.count == before)
  }

  @Test func timesOut() async {
    let quick = quick()
    quick.timeout = 0.2
    device.set(delay: .seconds(5))
    #expect(await quick.ask(request()) == .failure(.timeout))
  }

  @Test mutating func offOrChatProvider() async {
    on = []
    #expect(await quick().ask(request()) == .failure(.off))
    on = [ClaudeQuick.nameLinkFoldersKey]
    onDevice = false
    // The chat's provider, with a key: Claude's prompt goes to Claude.
    let quick = quick(key: "sk-ant-api03-test-key-1234")
    #expect(!quick.usesOnDevice(ClaudeQuick.nameLinkFoldersKey))
    _ = await quick.ask(request())
    #expect(device.asked.isEmpty)
    #expect(await fake.lastBody?["system"]?.array?.first?["text"] == "Claude's prompt.")
  }

  @Test func onDevicePrompts() {
    #expect(SidebarPrompts.section("a", in: "## a\nIn {language}.", locale: "pt_BR") == "In Portuguese.")
    #expect(SidebarPrompts.onDeviceSection("a", in: "## a\nClaude's.", locale: "en") == "Claude's.")
  }
}
