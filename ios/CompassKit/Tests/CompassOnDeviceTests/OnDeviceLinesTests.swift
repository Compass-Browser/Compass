// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Testing

@testable import CompassOnDevice

/// The helper's protocol, as the desktop's OnDeviceModel reads it
/// (browser/claude/on_device_model_unittest.cc).
struct OnDeviceLinesTests {
  let backend = OnDeviceBackend(
    availability: { .notReady },
    answer: { feature, system, user throws(OnDeviceFailure) in
      if system == "refuse" { throw .refused }
      return #"{"feature":"\#(feature)","user":"\#(user)"}"#
    })

  func reply(_ line: String) async -> [String: Any] {
    let text = await OnDeviceLines.handle(line, backend: backend)
    #expect(!text.contains("\n"))
    return (try? JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any]) ?? [:]
  }

  @Test func status() async {
    let status = await reply(#"{"id": 7, "status": true}"#)
    #expect(status["id"] as? Int == 7)
    #expect(status["availability"] as? String == "not_ready")
  }

  @Test func answers() async {
    let answer = await reply(#"{"id": "a", "feature": "folder_name", "system": "s", "user": "u"}"#)
    #expect(answer["id"] as? String == "a")
    #expect(answer["text"] as? String == #"{"feature":"folder_name","user":"u"}"#)
  }

  @Test func errors() async {
    let refused = await reply(#"{"id": 1, "feature": "folder_name", "system": "refuse", "user": "u"}"#)
    #expect(refused["error"] as? String == "refused")
    let missing = await reply(#"{"id": 2, "feature": "folder_name"}"#)
    #expect(missing["error"] as? String == "bad_request")
    #expect(missing["id"] as? Int == 2)
    let garbage = await reply("not json")
    #expect(garbage["error"] as? String == "bad_request")
    #expect(garbage["id"] is NSNull)
  }

  @Test func unknownFeature() async {
    await #expect(throws: OnDeviceFailure.unknownFeature) {
      try await OnDeviceModel.answer(feature: "command_bar_ask", system: "", user: "")
    }
  }

  @Test func reasons() {
    #expect(OnDeviceAvailability.available.reason == nil)
    #expect(OnDeviceAvailability.disabled.reason != nil)
    #expect(OnDeviceAvailability(rawValue: "not_ready") == .notReady)
  }
}
