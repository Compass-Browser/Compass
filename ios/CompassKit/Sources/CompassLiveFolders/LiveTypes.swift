// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// A JSON value: a manifest field's default, a config value.
public enum LiveJSON: Codable, Sendable, Equatable, Hashable {
  case string(String)
  case number(Double)
  case bool(Bool)
  case null

  public init(from decoder: Decoder) throws {
    let c = try decoder.singleValueContainer()
    if c.decodeNil() {
      self = .null
    } else if let b = try? c.decode(Bool.self) {
      self = .bool(b)
    } else if let n = try? c.decode(Double.self) {
      self = .number(n)
    } else if let s = try? c.decode(String.self) {
      self = .string(s)
    } else {
      self = .null  // objects and lists: no field type takes them
    }
  }

  public func encode(to encoder: Encoder) throws {
    var c = encoder.singleValueContainer()
    switch self {
    case .string(let s): try c.encode(s)
    case .number(let n): try c.encode(n)
    case .bool(let b): try c.encode(b)
    case .null: try c.encodeNil()
    }
  }

  /// As JSONSerialization gives and takes it.
  public var object: Any {
    switch self {
    case .string(let s): s
    case .number(let n): n == n.rounded() && abs(n) < 1e15 ? Int(n) as Any : n
    case .bool(let b): b
    case .null: NSNull()
    }
  }

  public init(object: Any?) {
    switch object {
    case let s as String: self = .string(s)
    case let n as NSNumber where CFGetTypeID(n) == CFBooleanGetTypeID(): self = .bool(n.boolValue)
    case let n as NSNumber: self = .number(n.doubleValue)
    default: self = .null
    }
  }

  public var text: String {
    switch self {
    case .string(let s): s
    case .number(let n): n == n.rounded() && abs(n) < 1e15 ? String(Int(n)) : String(n)
    case .bool(let b): b ? "true" : "false"
    case .null: ""
    }
  }
}

/// A plugin's manifest, as `plugins/runtime/manifest.js` validates and
/// normalizes it (docs/design/live-folders.md, "Manifest").
public struct LiveManifest: Codable, Sendable, Equatable {
  public struct Secret: Codable, Sendable, Equatable {
    public var key: String
    public var label: String
    public var optional: Bool
    public var help: String
  }

  public struct Option: Codable, Sendable, Equatable, Hashable {
    public var value: String
    public var label: String

    public init(from decoder: Decoder) throws {
      if let s = try? decoder.singleValueContainer().decode(String.self) {
        value = s
        label = s
        return
      }
      let c = try decoder.container(keyedBy: CodingKeys.self)
      value = try c.decode(String.self, forKey: .value)
      label = try c.decode(String.self, forKey: .label)
    }
  }

  public struct Field: Codable, Sendable, Equatable {
    /// string, url, number, bool or select.
    public var key: String
    public var type: String
    public var label: String
    public var `default`: LiveJSON?
    public var optional: Bool
    public var placeholder: String?
    public var help: String?
    public var min: Double?
    public var max: Double?
    public var options: [Option]?
  }

  public struct Refresh: Codable, Sendable, Equatable {
    public var `default`: Double
    public var min: Double
  }

  public var id: String
  public var name: String
  public var version: String
  public var api: Int
  public var entry: String
  public var icon: String?
  public var description: String
  public var hosts: [String]
  public var cookies: [String]
  public var secrets: [Secret]
  public var config: [Field]
  public var refresh: Refresh
}

/// A plugin ready to run: its manifest, entry script and icon.
public struct LivePlugin: Sendable, Equatable {
  public var manifest: LiveManifest
  public var source: String
  /// The icon file's bytes (SVG or PNG), if it has one.
  public var icon: Data?
  /// Where it was installed from, and the SHA-256 of what was installed
  /// (`LivePluginPackage.hash`); empty for a built-in or a local plugin.
  public var sourceURL: String
  public var sha256: String

  public init(manifest: LiveManifest, source: String, icon: Data? = nil, sourceURL: String = "", sha256: String = "") {
    self.manifest = manifest
    self.source = source
    self.icon = icon
    self.sourceURL = sourceURL
    self.sha256 = sha256
  }

  public var id: String { manifest.id }
  public var isBuiltin: Bool { manifest.id.hasPrefix("compass.") }
}

/// What a refresh returned, normalized (`plugins/runtime/result.js`).
public struct LiveResult: Codable, Sendable, Equatable {
  public struct Item: Codable, Sendable, Equatable {
    public var id: String
    public var title: String
    public var url: String
    public var subtitle: String?
    public var icon: String?
    /// Epoch ms.
    public var time: Double?
    public var badge: String?
  }

  public var items: [Item]
  /// A suggested folder name.
  public var title: String?
  /// Seconds until the next refresh, before clamping.
  public var nextRefresh: Double?
  public var warnings: [String]
}

/// Why a refresh failed. The codes are the contract
/// (docs/design/live-folders.md, "Runtime"); messages are for the user.
public struct LiveError: Error, Sendable, Equatable, CustomStringConvertible {
  public var code: String
  public var message: String

  public init(_ code: String, _ message: String) {
    self.code = code
    self.message = message
  }

  public var description: String { message }
}

/// The host's limits for one refresh.
public struct LiveLimits: Sendable, Equatable {
  public var timeout: TimeInterval = 30
  public var maxFetches = 10
  public var maxResponseBytes = 2 * 1024 * 1024
  public var maxRedirects = 5
  public var maxItems = 50

  public init() {}
}

/// What the plugin sees of the user: `compass.now()`, `locale`, `timeZone`.
public struct LiveEnvironment: Sendable, Equatable {
  public var now: Date
  public var locale: String
  public var timeZone: String

  public init(now: Date = Date(), locale: String? = nil, timeZone: String? = nil) {
    self.now = now
    // BCP 47, as Intl wants it ("en-US", not "en_US").
    self.locale = locale ?? Locale.current.identifier(.bcp47)
    self.timeZone = timeZone ?? TimeZone.current.identifier
  }
}

/// One request a plugin made, as the host checked and sent it.
public struct LiveFetchRequest: Sendable, Equatable {
  public var method: String
  public var url: URL
  /// Lower-case names; the forbidden ones are already dropped.
  public var headers: [String: String]
  public var body: String?
  /// Send the profile's cookies (the host and the user allow it).
  public var withCookies: Bool
}

public struct LiveFetchResponse: Sendable {
  public var status: Int
  /// After redirects.
  public var url: URL
  public var headers: [String: String]
  public var body: Data

  public init(status: Int, url: URL, headers: [String: String], body: Data) {
    self.status = status
    self.url = url
    self.headers = headers
    self.body = body
  }
}

/// The hosts a refresh may reach, placeholders resolved.
public struct LiveAllowedHosts: Sendable, Equatable {
  public var hosts: [String]
  public var cookies: [String]

  public init(hosts: [String], cookies: [String]) {
    self.hosts = hosts
    self.cookies = cookies
  }

  /// `manifest.js`'s hostMatches: "example.com" or "*.example.com" (the
  /// domain and its subdomains).
  public static func hostMatches(_ pattern: String, _ hostname: String) -> Bool {
    var host = hostname.lowercased()
    if host.hasSuffix(".") { host.removeLast() }
    if pattern.hasPrefix("*.") {
      let domain = String(pattern.dropFirst(2))
      return host == domain || host.hasSuffix("." + domain)
    }
    return host == pattern
  }

  /// `manifest.js`'s urlAllowed, for a URL Foundation parsed (a redirect).
  public func allows(_ url: URL) -> Bool {
    guard let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https",
      url.user == nil, url.password == nil, let host = url.host(percentEncoded: false), !host.isEmpty
    else { return false }
    return hosts.contains { Self.hostMatches($0, host) }
  }

  public func allowsCookies(_ url: URL) -> Bool {
    guard let host = url.host(percentEncoded: false) else { return false }
    return cookies.contains { Self.hostMatches($0, host) }
  }
}

/// Fetches for the runtime: the network (`URLSessionLiveFetcher`) or
/// recorded responses (tests).
public protocol LiveFetcher: Sendable {
  /// Throws a `LiveError` (host_not_allowed for a redirect off the
  /// allowlist, response_too_large, network).
  func fetch(_ request: LiveFetchRequest, allowed: LiveAllowedHosts, limits: LiveLimits) async throws
    -> LiveFetchResponse
}

/// A refresh's outcome. Never thrown: errors are `.failure`.
public struct LiveRun: Sendable {
  public struct Fetch: Sendable, Equatable {
    public var method: String
    public var url: String
    public var cookies: Bool
  }

  public var result: Result<LiveResult, LiveError>
  public var fetches: [Fetch]
  public var log: [String]
}
