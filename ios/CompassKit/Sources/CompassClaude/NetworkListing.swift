// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// iOS's network_requests and network_response (docs/design/claude.md,
/// "Developer tools", iOS): three sources merged, each marked, since none
/// sees everything the desktop's DevTools does.
/// - The page's own fetch, XHR, beacons, WebSockets and EventSources, from
///   the capture script: full detail (headers, bodies), but reported by
///   the page, which can hide or fake them. Ids "r<n>".
/// - Resource timings: everything the page loaded (documents, scripts,
///   styles, images, fonts), URL, type, size and time, no headers or
///   bodies, a status only where WebKit gives one. Ids "t<n>".
/// - The local proxy's connections: every connection the tab's website
///   data opened (host and port only, never their content: no TLS
///   interception), with errors the page never sees (DNS, refused,
///   timeouts). Ids "c<n>".
public enum NetworkListing {
  /// What an iOS listing leaves out, said once at its end.
  public static let iosNote =
    "On iOS: requests the page made itself (r) are reported by the page; other loads (t) come from the page's timings, without headers or bodies; connections (c) are every connection this tab's site data opened, host and port only."

  public struct PageRequest: Sendable, Equatable {
    public var seq: Int
    public var type: String
    public var method: String
    public var url: String
    public var status: Int?
    public var statusText: String
    public var error: String?
    public var start: Double
    public var end: Double?
    public var size: Double?
    public var mime: String
    public var requestHeaders: [(String, String)]
    public var requestBody: String?
    public var responseHeaders: [(String, String)]
    public var body: String?
    public var bodyLength: Int?
    public var bodyDropped: Bool
    public var opaque: Bool
    public var messages: (sent: Int, received: Int)?

    public static func == (a: PageRequest, b: PageRequest) -> Bool { a.seq == b.seq && a.url == b.url }

    public init?(_ value: JSONValue) {
      guard let seq = value["seq"]?.number, let url = value["url"]?.string else { return nil }
      self.seq = Int(seq)
      self.url = url
      type = value["type"]?.string ?? "Other"
      method = value["method"]?.string ?? "GET"
      status = value["status"]?.number.map(Int.init)
      statusText = value["statusText"]?.string ?? ""
      error = value["error"]?.string
      start = value["start"]?.number ?? 0
      end = value["end"]?.number
      size = value["size"]?.number
      mime = value["mime"]?.string ?? ""
      func pairs(_ key: String) -> [(String, String)] {
        (value[key]?.array ?? []).compactMap { pair in
          guard let list = pair.array, list.count == 2, let name = list[0].string, let value = list[1].string
          else { return nil }
          return (name, value)
        }
      }
      requestHeaders = pairs("requestHeaders")
      requestBody = value["requestBody"]?.string
      responseHeaders = pairs("responseHeaders")
      body = value["body"]?.string
      bodyLength = value["bodyLength"]?.number.map(Int.init)
      bodyDropped = value["bodyDropped"] == .bool(true)
      opaque = value["responseType"]?.string == "opaque"
      if let sent = value["sent"]?.number ?? (type == "EventSource" ? 0 : nil), let received = value["received"]?.number {
        messages = (Int(sent), Int(received))
      }
    }

    var failed: Bool { error != nil || (status ?? 0) >= 400 }
  }

  public struct Resource: Sendable, Equatable {
    public var seq: Int
    public var url: String
    /// The initiator type ("script", "img", "navigation"…).
    public var initiator: String
    public var start: Double
    public var duration: Double
    public var size: Double
    public var status: Int?

    public init(seq: Int, url: String, initiator: String, start: Double, duration: Double, size: Double, status: Int?) {
      self.seq = seq
      self.url = url
      self.initiator = initiator
      self.start = start
      self.duration = duration
      self.size = size
      self.status = status
    }

    /// The desktop's resource types, from WebKit's initiator types.
    public var type: String {
      switch initiator {
      case "navigation", "iframe", "frame": "Document"
      case "link", "css": url.lowercased().contains(".css") || initiator == "css" ? "Stylesheet" : "Other"
      case "script": "Script"
      case "img", "image", "imageset", "input", "svg": "Image"
      case "video", "audio", "track": "Media"
      case "fetch": "Fetch"
      case "xmlhttprequest": "XHR"
      case "beacon": "Ping"
      default: url.range(of: #"\.(woff2?|ttf|otf)(\?|$)"#, options: [.regularExpression, .caseInsensitive]) != nil ? "Font" : "Other"
      }
    }
  }

  public struct Connection: Sendable, Equatable {
    public var seq: Int
    public var host: String
    public var port: Int
    public var opened: Date
    /// Seconds from asking to connected; nil until then, or on failure.
    public var connectTime: Double?
    /// Seconds it lasted; nil while open.
    public var duration: Double?
    public var bytesUp: Int
    public var bytesDown: Int
    public var error: String?

    public init(
      seq: Int, host: String, port: Int, opened: Date, connectTime: Double?, duration: Double?, bytesUp: Int,
      bytesDown: Int, error: String?
    ) {
      self.seq = seq
      self.host = host
      self.port = port
      self.opened = opened
      self.connectTime = connectTime
      self.duration = duration
      self.bytesUp = bytesUp
      self.bytesDown = bytesDown
      self.error = error
    }
  }

  public struct Filter {
    public var onlyNew = false
    public var pattern: NSRegularExpression?
    public var types: Set<String> = []
    public var failed = false
    public var limit = 50

    public init(onlyNew: Bool = false, pattern: NSRegularExpression? = nil, types: Set<String> = [], failed: Bool = false, limit: Int = 50) {
      self.onlyNew = onlyNew
      self.pattern = pattern
      self.types = types
      self.failed = failed
      self.limit = limit
    }

    func matches(url: String, type: String, failed isFailed: Bool) -> Bool {
      if failed && !isFailed { return false }
      if !types.isEmpty && !types.contains(type) { return false }
      if let pattern, pattern.firstMatch(in: url, range: NSRange(url.startIndex..., in: url)) == nil { return false }
      return true
    }
  }

  /// What network_requests read last, for `only_new`.
  public struct Seen: Sendable, Equatable {
    public var request = 0
    public var resource = 0
    public var connection = 0
    public init(request: Int = 0, resource: Int = 0, connection: Int = 0) {
      self.request = request
      self.resource = resource
      self.connection = connection
    }
  }

  static let maxListedURL = 300

  /// Resource timings that the page's own log doesn't already list (its
  /// fetches and XHRs show there, with more detail), numbered from 1.
  public static func resources(from value: JSONValue?, besides requests: [PageRequest]) -> [Resource] {
    let logged = Set(requests.map(\.url))
    var seq = 0
    return (value?["list"]?.array ?? []).compactMap { item in
      seq += 1
      guard let url = item["url"]?.string else { return nil }
      let initiator = item["type"]?.string ?? "other"
      if ["fetch", "xmlhttprequest", "beacon"].contains(initiator), logged.contains(url) { return nil }
      let status = item["status"]?.number.flatMap { $0 > 0 ? Int($0) : nil }
      return Resource(
        seq: seq, url: url, initiator: initiator, start: item["start"]?.number ?? 0,
        duration: item["duration"]?.number ?? 0, size: item["size"]?.number ?? 0, status: status)
    }
  }

  /// network_requests' answer.
  public static func list(
    requests: [PageRequest], resources: [Resource], connections: [Connection], captured: Bool, proxyOn: Bool,
    filter: Filter, seen: inout Seen
  ) -> String {
    var lines: [(start: Double, text: String)] = []
    let fresh = filter.onlyNew ? seen : Seen()
    for request in requests where request.seq > fresh.request {
      guard filter.matches(url: request.url, type: request.type, failed: request.failed) else { continue }
      let status = request.error != nil ? "failed" : request.status.map(String.init) ?? "…"
      var text = "r\(request.seq) \(request.method) \(status) \(request.type) \(listedURL(request.url)) — "
      if let error = request.error {
        text += error
      } else if request.end == nil {
        text += request.type == "WebSocket" || request.type == "EventSource" ? "open" : "pending"
      } else {
        var details: [String] = []
        if !request.mime.isEmpty { details.append(request.mime) }
        if request.opaque {
          details.append("size unavailable (opaque)")
        } else if let size = request.size {
          details.append(DevToolsFormat.formatBytes(size))
        }
        if let end = request.end { details.append(DevToolsFormat.formatDuration((end - request.start) / 1000)) }
        text += details.joined(separator: ", ")
      }
      if let messages = request.messages {
        text += "; \(messages.sent) sent, \(messages.received) received"
      }
      lines.append((request.start, text))
    }
    for resource in resources where resource.seq > fresh.resource {
      let failed = (resource.status ?? 0) >= 400
      guard filter.matches(url: resource.url, type: resource.type, failed: failed) else { continue }
      let status = resource.status.map(String.init) ?? "status unavailable"
      let size = resource.size > 0 ? DevToolsFormat.formatBytes(resource.size) : "cache or size unavailable"
      lines.append(
        (resource.start,
         "t\(resource.seq) GET \(status) \(resource.type) \(listedURL(resource.url)) — \(size), \(DevToolsFormat.formatDuration(resource.duration / 1000))"))
    }
    var text = ""
    let matching = lines.sorted { $0.start < $1.start }
    let skipped = max(0, matching.count - filter.limit)
    if skipped > 0 { text += "\(skipped) older requests not shown.\n" }
    text += matching.suffix(filter.limit).map { $0.text + "\n" }.joined()

    // Connections: complete for the tab's site data, but not per request.
    // The URL and types filters don't apply to them (no URL, no type); a
    // host the pattern names does.
    let shownConnections = connections.filter { connection in
      guard connection.seq > fresh.connection else { return false }
      if filter.failed && connection.error == nil { return false }
      if let pattern = filter.pattern {
        let host = connection.host
        return pattern.firstMatch(in: host, range: NSRange(host.startIndex..., in: host)) != nil
      }
      return filter.types.isEmpty
    }
    if !shownConnections.isEmpty {
      text += "\nConnections (complete, from Compass's proxy for this tab's site data; host and port only):\n"
      let cut = max(0, shownConnections.count - filter.limit)
      if cut > 0 { text += "\(cut) older connections not shown.\n" }
      for connection in shownConnections.suffix(filter.limit) {
        text += "c\(connection.seq) \(connection.host):\(connection.port) — "
        if let error = connection.error {
          text += "failed: \(error)"
        } else {
          var details: [String] = []
          if let connect = connection.connectTime { details.append("connected in \(DevToolsFormat.formatDuration(connect))") }
          details.append("\(DevToolsFormat.formatBytes(Double(connection.bytesUp))) up, \(DevToolsFormat.formatBytes(Double(connection.bytesDown))) down")
          details.append(connection.duration.map { "lasted \(DevToolsFormat.formatDuration($0))" } ?? "open")
          text += details.joined(separator: ", ")
        }
        text += "\n"
      }
    } else if !proxyOn && !filter.onlyNew {
      text += "\nConnections: unavailable (Compass's proxy isn't running).\n"
    }

    seen = Seen(
      request: max(fresh.request, requests.map(\.seq).max() ?? 0),
      resource: max(fresh.resource, resources.map(\.seq).max() ?? 0),
      connection: max(fresh.connection, connections.map(\.seq).max() ?? 0))
    if matching.isEmpty && shownConnections.isEmpty {
      if filter.onlyNew { return "No new requests." }
      return captured
        ? "No requests recorded. Navigate with \"reload\" to see the page load."
        : "No requests recorded: the page loaded before Claude's developer tools were on. Navigate with \"reload\" to see the page load."
    }
    if !captured { text += "\nThe page's own requests (r) aren't recorded: it loaded before the developer tools were on. Reload to record them.\n" }
    return text + "\n" + iosNote
  }

  /// network_response's answer for `id`, or nil for an unknown id.
  public static func response(
    id: String, requests: [PageRequest], resources: [Resource], connections: [Connection], includeBody: Bool,
    offset: Int, showSecrets: Bool
  ) -> (text: String, isError: Bool) {
    let lower = id.lowercased()
    guard let kind = lower.first, let seq = Int(lower.dropFirst()) else {
      return ("No request \(id): network_requests gives the ids.", true)
    }
    let shownURL = { (url: String) in showSecrets ? url : DevToolsFormat.maskSecretsInText(url) }
    switch kind {
    case "t":
      guard let resource = resources.first(where: { $0.seq == seq }) else { break }
      return (
        "\(resource.type) \(shownURL(resource.url))\nStatus: \(resource.status.map(String.init) ?? "unavailable")\nSize: \(resource.size > 0 ? DevToolsFormat.formatBytes(resource.size) : "unavailable")\nTime: \(DevToolsFormat.formatDuration(resource.duration / 1000))\n\nHeaders and body: unavailable on iOS for loads the page didn't make with fetch or XMLHttpRequest. Use run_js with fetch() to read a URL the page may read.",
        false
      )
    case "c":
      guard let connection = connections.first(where: { $0.seq == seq }) else { break }
      var text = "Connection to \(connection.host):\(connection.port)\n"
      if let error = connection.error { text += "Failed: \(error)\n" }
      if let connect = connection.connectTime { text += "Connected in \(DevToolsFormat.formatDuration(connect))\n" }
      text += "Sent \(DevToolsFormat.formatBytes(Double(connection.bytesUp))), received \(DevToolsFormat.formatBytes(Double(connection.bytesDown)))\n"
      text += connection.duration.map { "Lasted \(DevToolsFormat.formatDuration($0))\n" } ?? "Still open\n"
      text += "\nIts requests and content: unavailable (Compass never decrypts connections)."
      return (text, false)
    case "r":
      guard let request = requests.first(where: { $0.seq == seq }) else { break }
      var text = "\(request.method) \(shownURL(request.url))\n"
      if let error = request.error {
        text += "Failed: \(error)\n"
      } else if let status = request.status {
        text += "Status: \(status) \(request.statusText)\n"
      }
      text += "Type: \(request.type) (reported by the page)\n"
      text += "\nRequest headers (those the page set; the browser adds others, unavailable on iOS):\n"
      text += request.requestHeaders.isEmpty ? "  none\n" : DevToolsFormat.formatHeaders(request.requestHeaders, showSecrets: showSecrets)
      if let body = request.requestBody {
        text += "\nRequest body:\n\(DevToolsFormat.cut(showSecrets ? body : DevToolsFormat.maskSecretsInText(body), 20000))\n"
      }
      if request.end != nil || request.status != nil {
        text += "\nResponse headers:\n"
        text += request.opaque
          ? "  unavailable (a cross-origin response the page can't read)\n"
          : request.responseHeaders.isEmpty ? "  none\n" : DevToolsFormat.formatHeaders(request.responseHeaders, showSecrets: showSecrets)
      }
      guard includeBody else { return (text, false) }
      if request.end == nil && request.error == nil {
        text += "\nThe body is still loading."
      } else if request.opaque {
        text += "\nThe body: unavailable (cross-origin)."
      } else if let body = request.body {
        let part = DevToolsFormat.part(showSecrets ? body : DevToolsFormat.maskSecretsInText(body), offset: offset, max: 20000)
        text += "\nResponse body (\(DevToolsFormat.formatBytes(Double(request.bodyLength ?? body.count)))\(offset > 0 ? ", from \(offset)" : "")):\n\(part.text)"
        if let next = part.next {
          text += "\n… more: offset \(next)"
        } else if let length = request.bodyLength, length > body.count {
          text += "\n… the rest (past 64 kB) isn't kept on iOS."
        }
      } else if request.bodyDropped {
        text += "\nThe body isn't kept anymore (newer bodies took its place)."
      } else if !request.mime.isEmpty && !DevToolsFormat.isTextMimeType(request.mime) {
        text += "\nA binary body (\(request.mime)): not shown."
      } else if request.error == nil {
        text += "\nAn empty body."
      }
      return (text, false)
    default:
      break
    }
    return ("No request \(id): network_requests gives the ids (they start over when the page loads again).", true)
  }

  /// A URL as the list shows it: secrets hidden, cut.
  static func listedURL(_ url: String) -> String {
    DevToolsFormat.cut(DevToolsFormat.maskSecretsInText(url), url.hasPrefix("data:") ? 100 : maxListedURL)
  }
}
