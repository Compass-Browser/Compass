// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// The Mac helper's protocol (tools/on_device/, the browser's
/// browser/claude/on_device_model.*): JSON, one object a line, over stdin
/// and stdout, a reply line for each request line, in order.
///
/// - `{"id": 1, "status": true}` → `{"id": 1, "availability": "available"}`
///   (or "unsupported", "disabled", "not_ready": `OnDeviceAvailability`).
/// - `{"id": 2, "feature": "folder_name", "system": "…", "user": "…"}` →
///   `{"id": 2, "text": "{\"name\":\"…\"}"}`, or `{"id": 2, "error":
///   "refused"}` (`OnDeviceFailure`'s raw values, or "bad_request").
///
/// `id` is echoed back as it came (any JSON value; null if missing).
public enum OnDeviceLines {
  /// The reply line to `line` (without its line break).
  public static func handle(_ line: String, backend: OnDeviceBackend = .system) async -> String {
    guard let data = line.data(using: .utf8),
      let request = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
    else {
      return encode(["id": NSNull(), "error": "bad_request"])
    }
    let id = request["id"] ?? NSNull()
    if request["status"] as? Bool == true {
      return encode(["id": id, "availability": backend.availability().rawValue])
    }
    guard let feature = request["feature"] as? String, let system = request["system"] as? String,
      let user = request["user"] as? String
    else {
      return encode(["id": id, "error": "bad_request"])
    }
    do {
      let text = try await backend.answer(feature, system, user)
      return encode(["id": id, "text": text])
    } catch {
      return encode(["id": id, "error": error.rawValue])
    }
  }

  /// Reads request lines from stdin until it closes, answering each on
  /// stdout: the helper's whole `main`.
  public static func serve(backend: OnDeviceBackend = .system) async {
    while let line = readLine(strippingNewline: true) {
      if line.trimmingCharacters(in: .whitespaces).isEmpty { continue }
      let reply = await handle(line, backend: backend)
      FileHandle.standardOutput.write(Data((reply + "\n").utf8))
    }
  }

  static func encode(_ object: [String: Any]) -> String {
    guard let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys, .fragmentsAllowed]),
      let text = String(data: data, encoding: .utf8)
    else { return #"{"error":"failed","id":null}"# }
    return text
  }
}
