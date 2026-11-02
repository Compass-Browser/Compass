// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import CompassShared
import Foundation

/// Talks to the Messages API over URLSession, as the desktop's ClaudeClient
/// does over SimpleURLLoader: one streamed request, its server-sent events
/// passed on as they come; and the model list. Raw HTTP: Swift has no
/// official Anthropic SDK.
enum ClaudeClient {
  /// Streams one reply. Each event goes to `onEvent`; the result is nil
  /// when the reply ended properly (or was cancelled), else what went
  /// wrong, for the user: an HTTP error (the API's own message), an `error`
  /// event, a network failure, or a stream that ended without
  /// `message_stop`.
  static func stream(
    _ request: ChatRequest, endpoint: ClaudeEndpoint, onEvent: @MainActor (String, String) -> Void
  ) async -> String? {
    var urlRequest = endpoint.request("v1/messages", body: request.body.encoded(), betas: request.betas)
    urlRequest.setValue("text/event-stream", forHTTPHeaderField: "accept")
    // A long reply streams for minutes; only silence counts.
    urlRequest.timeoutInterval = 300
    do {
      let (bytes, response) = try await URLSession.shared.bytes(for: urlRequest)
      let status = (response as? HTTPURLResponse)?.statusCode ?? 0
      guard status == 200 else {
        var body = Data()
        for try await byte in bytes {
          body.append(byte)
          if body.count > 64 * 1024 { break }
        }
        return ClaudeEndpoint.describeError(status: status, body: body)
      }
      var parser = EventStreamParser(maxLine: 4 << 20)
      var stopped = false
      for try await byte in bytes {
        for event in parser.feed(byte) {
          switch event.name {
          case "error":
            return JSONValue.parse(event.data)?["error"]?["message"]?.string ?? "The reply failed."
          case "message_stop":
            stopped = true
          default:
            break
          }
          await onEvent(event.name, event.data)
        }
      }
      return stopped || Task.isCancelled ? nil : "The reply ended early. Try again."
    } catch is CancellationError {
      return nil
    } catch let error as URLError where error.code == .cancelled {
      return nil
    } catch {
      return ClaudeEndpoint.describeError(status: 0, body: Data())
    }
  }

  /// `GET /v1/models`: what the key can use ("Test connection").
  static func models(endpoint: ClaudeEndpoint) async -> Result<[ClaudeModel], ClaudeError> {
    do {
      let (data, response) = try await URLSession.shared.data(for: endpoint.request("v1/models?limit=100"))
      let status = (response as? HTTPURLResponse)?.statusCode ?? 0
      guard status == 200 else { return .failure(ClaudeError(ClaudeEndpoint.describeError(status: status, body: data))) }
      guard let models = ClaudeEndpoint.parseModels(data) else { return .failure(ClaudeError("The server's answer wasn't a model list.")) }
      return .success(models)
    } catch {
      return .failure(ClaudeError(ClaudeEndpoint.describeError(status: 0, body: Data())))
    }
  }
}

struct ClaudeError: Error {
  let message: String
  init(_ message: String) { self.message = message }
}
