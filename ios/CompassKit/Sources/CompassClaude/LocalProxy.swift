// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Network
import Security

/// The proxy behind iOS's network_requests (docs/design/claude.md,
/// "Developer tools", iOS): while the developer tools are on, the website
/// data stores Claude may use send every connection through it
/// (`WKWebsiteDataStore.proxyConfigurations`), so Claude sees each one:
/// host, port, timings, bytes and errors (DNS, refused, timeouts) the page
/// never sees.
/// - HTTP CONNECT (and plain absolute-form requests), byte pumping only:
///   TLS is never intercepted, so it reads nothing of what's sent.
/// - On 127.0.0.1 only, local connections only, and each connection must
///   carry the run's random password (Proxy-Authorization: Basic): other
///   apps on the device can't use it. The user name says which data store
///   the connection is for.
/// - Each send waits for the previous one, so nothing piles up in memory,
///   and nothing is added in between: no noticeable latency.
/// - It can also slow a store's connections down or refuse them (emulate's
///   network conditions).
public final class LocalProxy: @unchecked Sendable {
  public struct Conditions: Sendable, Equatable {
    public var offline: Bool
    /// Added before each connection, in seconds.
    public var latency: Double
    /// Bytes a second; nil for no limit.
    public var download: Double?
    public var upload: Double?

    public init(offline: Bool = false, latency: Double = 0, download: Double? = nil, upload: Double? = nil) {
      self.offline = offline
      self.latency = latency
      self.download = download
      self.upload = upload
    }

    /// emulate's presets (the desktop's kNetworks); nil for "none".
    public static func preset(_ name: String) -> Conditions? {
      switch name {
      case "offline": Conditions(offline: true)
      case "slow_3g": Conditions(latency: 2, download: 50_000, upload: 50_000)
      case "fast_3g": Conditions(latency: 0.563, download: 180_000, upload: 84_000)
      case "slow_4g": Conditions(latency: 0.15, download: 1_600_000, upload: 750_000)
      default: nil
      }
    }
  }

  /// Connections kept per store, the oldest dropped first.
  public static let maxConnections = 500
  static let maxHeader = 16 * 1024
  static let connectTimeout = 30

  private let queue = DispatchQueue(label: "org.compassbrowser.devtools-proxy")
  private var listener: NWListener?
  private var port: UInt16?
  public let password: String
  private var logs: [String: [Record]] = [:]
  private var seqs: [String: Int] = [:]
  private var conditions: [String: Conditions] = [:]
  private var sessions: Set<Session> = []

  final class Record {
    let seq: Int
    let host: String
    let port: Int
    let opened = Date()
    var connected: Date?
    var closed: Date?
    var bytesUp = 0
    var bytesDown = 0
    var error: String?

    init(seq: Int, host: String, port: Int) {
      self.seq = seq
      self.host = host
      self.port = port
    }

    var connection: NetworkListing.Connection {
      NetworkListing.Connection(
        seq: seq, host: host, port: port, opened: opened,
        connectTime: connected.map { $0.timeIntervalSince(opened) }, duration: closed.map { $0.timeIntervalSince(opened) },
        bytesUp: bytesUp, bytesDown: bytesDown, error: error)
    }
  }

  public init() {
    var bytes = [UInt8](repeating: 0, count: 24)
    _ = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
    password = bytes.map { String(format: "%02x", $0) }.joined()
  }

  /// Starts listening (once); the port it listens on.
  public func start() async throws -> UInt16 {
    if let port = queue.sync(execute: { self.port }) { return port }
    let parameters = NWParameters.tcp
    parameters.acceptLocalOnly = true
    parameters.requiredLocalEndpoint = .hostPort(host: "127.0.0.1", port: .any)
    let listener = try NWListener(using: parameters)
    listener.newConnectionHandler = { [weak self] connection in self?.accept(connection) }
    return try await withCheckedThrowingContinuation { continuation in
      // On `queue`, once.
      final class Once: @unchecked Sendable { var done = false }
      let once = Once()
      listener.stateUpdateHandler = { [weak self] state in
        guard !once.done else { return }
        switch state {
        case .ready:
          once.done = true
          let port = listener.port?.rawValue ?? 0
          self?.port = port
          continuation.resume(returning: port)
        case .failed(let error):
          once.done = true
          continuation.resume(throwing: error)
        case .cancelled:
          once.done = true
          continuation.resume(throwing: NWError.posix(.ECANCELED))
        default:
          break
        }
      }
      queue.sync { self.listener = listener }
      listener.start(queue: queue)
    }
  }

  /// Stops listening and ends every tunnel; the logs stay.
  public func stop() {
    queue.sync {
      listener?.cancel()
      listener = nil
      port = nil
      for session in sessions { session.finish() }
      sessions = []
    }
  }

  public var isRunning: Bool { queue.sync { port != nil } }

  /// The connections of `store`'s website data, oldest first.
  public func connections(store: String) -> [NetworkListing.Connection] {
    queue.sync { (logs[store] ?? []).map(\.connection) }
  }

  /// Slows down or refuses `store`'s new connections; nil for none.
  public func setConditions(_ conditions: Conditions?, store: String) {
    queue.sync { self.conditions[store] = conditions }
  }

  /// The Proxy-Authorization value for `store` (tests; WebKit sends it from
  /// the proxy configuration's credential).
  public func authorization(store: String) -> String {
    "Basic " + Data("\(store):\(password)".utf8).base64EncodedString()
  }

  // MARK: Connections (on `queue`)

  private func accept(_ client: NWConnection) {
    let session = Session(proxy: self, client: client)
    sessions.insert(session)
    session.start()
  }

  fileprivate func ended(_ session: Session) { sessions.remove(session) }

  fileprivate func record(store: String, host: String, port: Int) -> Record {
    let seq = (seqs[store] ?? 0) + 1
    seqs[store] = seq
    let record = Record(seq: seq, host: host, port: port)
    var log = logs[store] ?? []
    log.append(record)
    if log.count > Self.maxConnections { log.removeFirst(log.count - Self.maxConnections) }
    logs[store] = log
    return record
  }

  fileprivate func store(for authorization: String?) -> String? {
    guard let authorization, authorization.lowercased().hasPrefix("basic "),
      let data = Data(base64Encoded: String(authorization.dropFirst(6)).trimmingCharacters(in: .whitespaces)),
      let text = String(data: data, encoding: .utf8), let colon = text.lastIndex(of: ":")
    else { return nil }
    // Constant time, so the password can't be guessed a byte at a time.
    let given = Array(text[text.index(after: colon)...].utf8)
    let expected = Array(password.utf8)
    guard given.count == expected.count else { return nil }
    var difference: UInt8 = 0
    for (a, b) in zip(given, expected) { difference |= a ^ b }
    return difference == 0 ? String(text[..<colon]) : nil
  }

  fileprivate func conditions(for store: String) -> Conditions? { conditions[store] }

  fileprivate var dispatchQueue: DispatchQueue { queue }

  /// What the user and Claude read for a failed connection.
  static func describe(_ error: NWError) -> String {
    switch error {
    case .dns: return "DNS: the host wasn't found"
    case .posix(let code):
      switch code {
      case .ECONNREFUSED: return "refused"
      case .ETIMEDOUT: return "timed out"
      case .ENETUNREACH, .EHOSTUNREACH: return "unreachable"
      case .ECONNRESET: return "reset"
      default: return "\(code)"
      }
    case .tls(let status): return "TLS error \(status)"
    default: return error.localizedDescription
    }
  }
}

/// One client connection: its request head, then a tunnel to the host.
private final class Session: Hashable, @unchecked Sendable {
  weak var proxy: LocalProxy?
  let client: NWConnection
  var upstream: NWConnection?
  var record: LocalProxy.Record?
  var conditions: LocalProxy.Conditions?
  var head = Data()
  var finished = false
  var halfClosed = 0

  init(proxy: LocalProxy, client: NWConnection) {
    self.proxy = proxy
    self.client = client
  }

  static func == (a: Session, b: Session) -> Bool { a === b }
  func hash(into hasher: inout Hasher) { hasher.combine(ObjectIdentifier(self)) }

  var queue: DispatchQueue? { proxy?.dispatchQueue }

  func start() {
    guard let queue else { return }
    client.stateUpdateHandler = { [weak self] state in
      if case .failed = state { self?.finish() }
      if case .cancelled = state { self?.finish() }
    }
    client.start(queue: queue)
    readHead()
  }

  private func readHead() {
    client.receive(minimumIncompleteLength: 1, maximumLength: 8192) { [weak self] data, _, complete, error in
      guard let self else { return }
      if let data { self.head.append(data) }
      if let end = self.head.range(of: Data("\r\n\r\n".utf8)) {
        self.handle(head: self.head[..<end.lowerBound], rest: self.head[end.upperBound...])
      } else if complete || error != nil || self.head.count > LocalProxy.maxHeader {
        self.reply("400 Bad Request")
      } else {
        self.readHead()
      }
    }
  }

  private func handle(head: Data, rest: Data) {
    guard let proxy, let text = String(data: head, encoding: .utf8) else { return reply("400 Bad Request") }
    let lines = text.components(separatedBy: "\r\n")
    let parts = lines[0].split(separator: " ")
    guard parts.count == 3 else { return reply("400 Bad Request") }
    var authorization: String?
    for line in lines.dropFirst() {
      guard let colon = line.firstIndex(of: ":") else { continue }
      if line[..<colon].trimmingCharacters(in: .whitespaces).lowercased() == "proxy-authorization" {
        authorization = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
      }
    }
    guard let store = proxy.store(for: authorization) else {
      return reply("407 Proxy Authentication Required", headers: "Proxy-Authenticate: Basic realm=\"Compass\"\r\n")
    }
    let method = parts[0].uppercased()
    let host: String
    let port: Int
    var forward = Data()
    if method == "CONNECT" {
      let target = String(parts[1])
      guard let colon = target.lastIndex(of: ":"), let number = Int(target[target.index(after: colon)...]) else {
        return reply("400 Bad Request")
      }
      host = String(target[..<colon]).trimmingCharacters(in: CharacterSet(charactersIn: "[]"))
      port = number
    } else {
      // An absolute-form request (http://host/…): passed on as is, minus
      // the proxy's own header.
      guard let url = URL(string: String(parts[1])), let urlHost = url.host, url.scheme?.lowercased() == "http" else {
        return reply("400 Bad Request")
      }
      host = urlHost
      port = url.port ?? 80
      let kept = lines.filter { !$0.lowercased().hasPrefix("proxy-authorization:") && !$0.lowercased().hasPrefix("proxy-connection:") }
      forward = Data((kept.joined(separator: "\r\n") + "\r\n\r\n").utf8)
    }
    forward.append(rest)
    let firstBytes = forward
    let record = proxy.record(store: store, host: host, port: port)
    self.record = record
    conditions = proxy.conditions(for: store)
    if conditions?.offline == true {
      record.error = "offline (emulated)"
      record.closed = Date()
      return reply("502 Bad Gateway")
    }
    let tunnel = method == "CONNECT"
    let open: @Sendable () -> Void = { [weak self] in self?.connect(host: host, port: port, tunnel: tunnel, forward: firstBytes) }
    if let latency = conditions?.latency, latency > 0 {
      queue?.asyncAfter(deadline: .now() + latency, execute: open)
    } else {
      open()
    }
  }

  private func connect(host: String, port: Int, tunnel: Bool, forward: Data) {
    guard !finished, let queue, let nwPort = NWEndpoint.Port(rawValue: UInt16(clamping: port)) else { return reply("400 Bad Request") }
    let tcp = NWProtocolTCP.Options()
    tcp.connectionTimeout = LocalProxy.connectTimeout
    tcp.noDelay = true
    let upstream = NWConnection(host: NWEndpoint.Host(host), port: nwPort, using: NWParameters(tls: nil, tcp: tcp))
    self.upstream = upstream
    upstream.stateUpdateHandler = { [weak self] state in
      guard let self, !self.finished else { return }
      switch state {
      case .ready:
        self.record?.connected = Date()
        let begin: @Sendable () -> Void = { [weak self] in
          guard let self else { return }
          if !forward.isEmpty { self.send(forward, to: upstream, up: true) {} }
          self.pump(from: self.client, to: upstream, up: true)
          self.pump(from: upstream, to: self.client, up: false)
        }
        if tunnel {
          self.client.send(
            content: Data("HTTP/1.1 200 Connection Established\r\n\r\n".utf8),
            completion: .contentProcessed { error in error == nil ? begin() : self.finish() })
        } else {
          begin()
        }
      case .waiting(let error), .failed(let error):
        self.record?.error = LocalProxy.describe(error)
        self.reply("502 Bad Gateway")
      default:
        break
      }
    }
    upstream.start(queue: queue)
  }

  private func send(_ data: Data, to connection: NWConnection, up: Bool, then next: @escaping @Sendable () -> Void) {
    connection.send(content: data, completion: .contentProcessed { [weak self] error in
      guard let self else { return }
      if error != nil { return self.finish() }
      if up { self.record?.bytesUp += data.count } else { self.record?.bytesDown += data.count }
      // Throttled: wait as long as the bytes take at the emulated speed.
      let rate = up ? self.conditions?.upload : self.conditions?.download
      if let rate, rate > 0 {
        self.queue?.asyncAfter(deadline: .now() + Double(data.count) / rate, execute: next)
      } else {
        next()
      }
    })
  }

  private func pump(from source: NWConnection, to destination: NWConnection, up: Bool) {
    let throttled = (up ? conditions?.upload : conditions?.download) != nil
    source.receive(minimumIncompleteLength: 1, maximumLength: throttled ? 8192 : 65536) { [weak self] data, _, complete, error in
      guard let self, !self.finished else { return }
      let failed = error != nil
      let next: @Sendable () -> Void = { [weak self] in
        guard let self, !self.finished else { return }
        if complete || failed {
          destination.send(content: nil, contentContext: .finalMessage, isComplete: true, completion: .idempotent)
          self.halfClosed += 1
          if self.halfClosed == 2 || failed { self.finish() }
        } else {
          self.pump(from: source, to: destination, up: up)
        }
      }
      if let data, !data.isEmpty {
        self.send(data, to: destination, up: up, then: next)
      } else {
        next()
      }
    }
  }

  private func reply(_ status: String, headers: String = "") {
    guard !finished else { return }
    let text = "HTTP/1.1 \(status)\r\n\(headers)Content-Length: 0\r\nConnection: close\r\n\r\n"
    client.send(content: Data(text.utf8), completion: .contentProcessed { [weak self] _ in self?.finish() })
  }

  func finish() {
    guard !finished else { return }
    finished = true
    if let record, record.closed == nil { record.closed = Date() }
    client.cancel()
    upstream?.cancel()
    proxy?.ended(self)
  }
}
