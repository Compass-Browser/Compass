// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Network
import Testing

@testable import CompassClaude

/// iOS's developer tools (docs/design/claude.md, "Developer tools", iOS):
/// secrets, the merged network list, and the local proxy.
struct DevToolsFormatTests {
  @Test func secrets() {
    #expect(DevToolsFormat.isSecretName("Authorization"))
    #expect(DevToolsFormat.isSecretName("x-csrf-token"))
    #expect(DevToolsFormat.isSecretName("SESSIONID"))
    #expect(!DevToolsFormat.isSecretName("content-type"))
    #expect(DevToolsFormat.looksSecret("Bearer abc"))
    #expect(DevToolsFormat.looksSecret("eyJhbGciOi.eyJzdWIiOiIx.c2ln"))
    #expect(!DevToolsFormat.looksSecret("text/html"))
    #expect(DevToolsFormat.hidden("abcd") == "<hidden, 4 characters>")
    #expect(DevToolsFormat.hidden("a") == "<hidden, 1 character>")
    #expect(
      DevToolsFormat.maskSecretsInText("https://x.test/a?access_token=123&q=cats")
        == "https://x.test/a?access_token=<hidden>&q=cats")
    #expect(
      DevToolsFormat.maskSecretsInText(#"{"password": "hunter2", "user": "ann"}"#)
        == #"{"password": "<hidden>", "user": "ann"}"#)
  }

  @Test func headers() {
    let text = DevToolsFormat.formatHeaders(
      [("Content-Type", "application/json"), ("Authorization", "Bearer xyz"), ("Accept", "*/*")], showSecrets: false)
    #expect(text == "  accept: */*\n  authorization: <hidden, 10 characters>\n  content-type: application/json\n")
    #expect(DevToolsFormat.formatHeaders([("Authorization", "Bearer xyz")], showSecrets: true).contains("Bearer xyz"))
  }

  @Test func sizesTimesParts() {
    #expect(DevToolsFormat.formatBytes(512) == "512 B")
    #expect(DevToolsFormat.formatBytes(3200) == "3.2 kB")
    #expect(DevToolsFormat.formatDuration(0.12) == "120 ms")
    #expect(DevToolsFormat.formatDuration(1.44) == "1.4 s")
    #expect(DevToolsFormat.ms(-1) == "unavailable")
    #expect(DevToolsFormat.rate(2000, good: 2500, poor: 4000) == "good")
    let part = DevToolsFormat.part("abcdef", offset: 2, max: 3)
    #expect(part.text == "cde" && part.next == 5)
    #expect(DevToolsFormat.part("abcdef", offset: 5, max: 3).next == nil)
    #expect(DevToolsFormat.isTextMimeType("application/json; charset=utf-8"))
    #expect(!DevToolsFormat.isTextMimeType("image/png"))
  }

  @Test func cookies() {
    let cookie = DevToolsFormat.Cookie(
      name: "sid", value: "s3cret", domain: ".example.com", path: "/", expires: nil, httpOnly: true, secure: true,
      sameSite: "Lax")
    #expect(cookie.applies(to: "www.example.com"))
    #expect(cookie.applies(to: "example.com"))
    #expect(!cookie.applies(to: "notexample.com"))
    #expect(cookie.line(showValues: false) == "  sid=<hidden, 6 characters>; domain .example.com; path /; session; HttpOnly; Secure; SameSite=Lax\n")
    #expect(cookie.line(showValues: true).hasPrefix("  sid=s3cret;"))
  }

  @Test func devtoolsContextLine() {
    func context(_ on: Bool) -> String {
      ChatRequest.context(ChatRequest.Options(model: "m", systemPrompt: "P", claudeAPI: true, devtools: on))
    }
    #expect(context(true).contains("Developer tools are on"))
    #expect(context(false).contains("Developer tools are off"))
  }
}

struct NetworkListingTests {
  private func page() -> [NetworkListing.PageRequest] {
    let log = JSONValue.parse(
      #"""
      {"total": 2, "entries": [
        {"seq": 1, "type": "Fetch", "method": "POST", "url": "https://api.test/login?token=abc", "start": 10, "end": 70,
         "status": 200, "statusText": "OK", "mime": "application/json", "size": 30,
         "requestHeaders": [["content-type", "application/json"], ["authorization", "Bearer t"]],
         "requestBody": "{\"password\": \"pw\"}",
         "responseHeaders": [["content-type", "application/json"]], "body": "{\"ok\": true}", "bodyLength": 12},
        {"seq": 2, "type": "XHR", "method": "GET", "url": "https://api.test/missing", "start": 80, "end": 90,
         "status": 404, "statusText": "Not Found", "mime": "text/plain", "responseHeaders": []}
      ]}
      """#)
    return (log?["entries"]?.array ?? []).compactMap(NetworkListing.PageRequest.init)
  }

  private func timings(besides requests: [NetworkListing.PageRequest]) -> [NetworkListing.Resource] {
    let value = JSONValue.parse(
      #"""
      {"origin": 0, "list": [
        {"url": "https://site.test/", "type": "navigation", "start": 0, "duration": 120, "size": 5000},
        {"url": "https://site.test/app.js", "type": "script", "start": 20, "duration": 40, "size": 2000},
        {"url": "https://api.test/login?token=abc", "type": "fetch", "start": 10, "duration": 60, "size": 30}
      ]}
      """#)
    return NetworkListing.resources(from: value, besides: requests)
  }

  @Test func mergesTheSources() {
    let requests = page()
    let resources = timings(besides: requests)
    // The fetch the page logged isn't listed twice.
    #expect(resources.map(\.url) == ["https://site.test/", "https://site.test/app.js"])
    #expect(resources.map(\.type) == ["Document", "Script"])
    let connection = NetworkListing.Connection(
      seq: 1, host: "api.test", port: 443, opened: Date(), connectTime: 0.05, duration: nil, bytesUp: 900,
      bytesDown: 4000, error: nil)
    let failed = NetworkListing.Connection(
      seq: 2, host: "gone.test", port: 443, opened: Date(), connectTime: nil, duration: 0.01, bytesUp: 0, bytesDown: 0,
      error: "DNS: the host wasn't found")
    var seen = NetworkListing.Seen()
    let text = NetworkListing.list(
      requests: requests, resources: resources, connections: [connection, failed], captured: true, proxyOn: true,
      filter: .init(), seen: &seen)
    let lines = text.split(separator: "\n").map(String.init)
    // By start time: the document, the fetch, the script, the XHR.
    #expect(lines[0].hasPrefix("t1 GET status unavailable Document https://site.test/"))
    #expect(lines[1].hasPrefix("r1 POST 200 Fetch https://api.test/login?token=<hidden> — application/json, 30 B, 60 ms"))
    #expect(lines[2].hasPrefix("t2 GET status unavailable Script"))
    #expect(lines[3].hasPrefix("r2 GET 404 XHR"))
    #expect(text.contains("c1 api.test:443 — connected in 50 ms, 900 B up, 4.0 kB down, open"))
    #expect(text.contains("c2 gone.test:443 — failed: DNS: the host wasn't found"))
    #expect(text.hasSuffix(NetworkListing.iosNote))

    // Only new: nothing since.
    var only = NetworkListing.Filter()
    only.onlyNew = true
    #expect(
      NetworkListing.list(
        requests: requests, resources: resources, connections: [connection], captured: true, proxyOn: true,
        filter: only, seen: &seen) == "No new requests.")

    // Failed only: the 404 and the DNS failure.
    var failedOnly = NetworkListing.Filter()
    failedOnly.failed = true
    var fresh = NetworkListing.Seen()
    let failures = NetworkListing.list(
      requests: requests, resources: resources, connections: [connection, failed], captured: true, proxyOn: true,
      filter: failedOnly, seen: &fresh)
    #expect(failures.contains("r2 GET 404"))
    #expect(!failures.contains("r1 "))
    #expect(failures.contains("c2 gone.test"))
    #expect(!failures.contains("c1 api.test"))
  }

  @Test func notCapturedSaysToReload() {
    var seen = NetworkListing.Seen()
    let text = NetworkListing.list(
      requests: [], resources: [], connections: [], captured: false, proxyOn: false, filter: .init(), seen: &seen)
    #expect(text.contains("loaded before Claude's developer tools were on"))
  }

  @Test func responses() {
    let requests = page()
    let resources = timings(besides: requests)
    let hidden = NetworkListing.response(
      id: "r1", requests: requests, resources: resources, connections: [], includeBody: true, offset: 0,
      showSecrets: false)
    #expect(!hidden.isError)
    #expect(hidden.text.contains("authorization: <hidden, 8 characters>"))
    #expect(hidden.text.contains(#"{"password": "<hidden>"}"#))
    #expect(hidden.text.contains("Response body (12 B):\n{\"ok\": true}"))
    #expect(hidden.text.contains("reported by the page"))
    let shown = NetworkListing.response(
      id: "r1", requests: requests, resources: resources, connections: [], includeBody: false, offset: 0,
      showSecrets: true)
    #expect(shown.text.contains("Bearer t"))
    #expect(!shown.text.contains("Response body"))
    let timing = NetworkListing.response(
      id: "t2", requests: requests, resources: resources, connections: [], includeBody: true, offset: 0,
      showSecrets: false)
    #expect(timing.text.contains("Headers and body: unavailable on iOS"))
    #expect(NetworkListing.response(
      id: "r9", requests: requests, resources: resources, connections: [], includeBody: true, offset: 0,
      showSecrets: false).isError)
  }
}

/// The proxy, end to end on the Mac: a local server, a CONNECT through it.
@Suite(.serialized)
struct LocalProxyTests {
  /// A server on 127.0.0.1 that answers each connection's first bytes
  /// with "echo:" and them.
  final class EchoServer: @unchecked Sendable {
    let listener: NWListener
    let queue = DispatchQueue(label: "echo")
    var port: UInt16 = 0

    init() async throws {
      let parameters = NWParameters.tcp
      parameters.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: .any)
      listener = try NWListener(using: parameters)
      listener.newConnectionHandler = { [queue] connection in
        connection.start(queue: queue)
        connection.receive(minimumIncompleteLength: 1, maximumLength: 1000) { data, _, _, _ in
          connection.send(content: Data("echo:".utf8) + (data ?? Data()), completion: .contentProcessed { _ in })
        }
      }
      let listener = self.listener
      port = try await withCheckedThrowingContinuation { continuation in
        final class Once: @unchecked Sendable { var done = false }
        let once = Once()
        listener.stateUpdateHandler = { state in
          guard !once.done else { return }
          if case .ready = state {
            once.done = true
            continuation.resume(returning: listener.port?.rawValue ?? 0)
          }
          if case .failed(let error) = state {
            once.done = true
            continuation.resume(throwing: error)
          }
        }
        listener.start(queue: queue)
      }
    }

    deinit { listener.cancel() }
  }

  /// Sends `request` to the proxy, then `then` once the head's answer
  /// came, and returns everything read (within a second of quiet).
  private func exchange(port: UInt16, _ request: String, then more: String? = nil) async throws -> String {
    let connection = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: port)!, using: .tcp)
    let queue = DispatchQueue(label: "client")
    connection.start(queue: queue)
    defer { connection.cancel() }
    func send(_ text: String) async throws {
      try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
        connection.send(content: Data(text.utf8), completion: .contentProcessed { error in
          if let error { continuation.resume(throwing: error) } else { continuation.resume() }
        })
      }
    }
    func receive() async -> String {
      await withCheckedContinuation { continuation in
        connection.receive(minimumIncompleteLength: 1, maximumLength: 10000) { data, _, _, _ in
          continuation.resume(returning: String(decoding: data ?? Data(), as: UTF8.self))
        }
      }
    }
    try await send(request)
    var text = await receive()
    if let more {
      try await send(more)
      text += await receive()
    }
    return text
  }

  @Test func tunnelsAndRecords() async throws {
    let server = try await EchoServer()
    let proxy = LocalProxy()
    let port = try await proxy.start()
    defer { proxy.stop() }
    let auth = proxy.authorization(store: "home")
    let text = try await exchange(
      port: port, "CONNECT 127.0.0.1:\(server.port) HTTP/1.1\r\nHost: x\r\nProxy-Authorization: \(auth)\r\n\r\n",
      then: "hello")
    #expect(text.hasPrefix("HTTP/1.1 200 Connection Established\r\n\r\n"))
    #expect(text.hasSuffix("echo:hello"))
    try await Task.sleep(for: .milliseconds(200))
    let log = proxy.connections(store: "home")
    #expect(log.count == 1)
    #expect(log.first?.host == "127.0.0.1")
    #expect(log.first?.port == Int(server.port))
    #expect(log.first?.bytesUp == 5)
    #expect(log.first?.bytesDown == 10)
    #expect(log.first?.connectTime != nil)
    #expect(proxy.connections(store: "other").isEmpty)
  }

  @Test func refusesWithoutThePassword() async throws {
    let proxy = LocalProxy()
    let port = try await proxy.start()
    defer { proxy.stop() }
    let none = try await exchange(port: port, "CONNECT example.com:443 HTTP/1.1\r\n\r\n")
    #expect(none.hasPrefix("HTTP/1.1 407"))
    #expect(none.contains("Proxy-Authenticate: Basic"))
    let wrong = "Basic " + Data("home:nope".utf8).base64EncodedString()
    let bad = try await exchange(port: port, "CONNECT example.com:443 HTTP/1.1\r\nProxy-Authorization: \(wrong)\r\n\r\n")
    #expect(bad.hasPrefix("HTTP/1.1 407"))
    #expect(proxy.connections(store: "home").isEmpty)
  }

  @Test func recordsFailuresAndOffline() async throws {
    // A port nothing listens on: refused.
    let closed = try await EchoServer()
    let closedPort = closed.port
    closed.listener.cancel()
    try await Task.sleep(for: .milliseconds(100))
    let proxy = LocalProxy()
    let port = try await proxy.start()
    defer { proxy.stop() }
    let auth = proxy.authorization(store: "home")
    let refused = try await exchange(port: port, "CONNECT 127.0.0.1:\(closedPort) HTTP/1.1\r\nProxy-Authorization: \(auth)\r\n\r\n")
    #expect(refused.hasPrefix("HTTP/1.1 502"))
    #expect(proxy.connections(store: "home").last?.error == "refused")

    proxy.setConditions(LocalProxy.Conditions.preset("offline"), store: "home")
    let offline = try await exchange(port: port, "CONNECT example.com:443 HTTP/1.1\r\nProxy-Authorization: \(auth)\r\n\r\n")
    #expect(offline.hasPrefix("HTTP/1.1 502"))
    #expect(proxy.connections(store: "home").last?.error == "offline (emulated)")
    #expect(LocalProxy.Conditions.preset("none") == nil)
  }

  @Test func startsWithARandomPassword() async throws {
    let proxy = LocalProxy()
    _ = try await proxy.start()
    defer { proxy.stop() }
    #expect(proxy.isRunning)
    #expect(proxy.password.count == 48)
  }
}
