// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// The filter lists content blocking downloads, and keeps up to date.
public enum FilterList: String, CaseIterable, Sendable, Codable {
  case easyList, easyPrivacy, cookies

  /// Where it comes from. Changing one here is all a new source takes.
  public var source: URL {
    switch self {
    case .easyList: URL(string: "https://easylist.to/easylist/easylist.txt")!
    case .easyPrivacy: URL(string: "https://easylist.to/easylist/easyprivacy.txt")!
    case .cookies: URL(string: "https://secure.fanboy.co.nz/fanboy-cookiemonster.txt")!
    }
  }

  public var name: String {
    switch self {
    case .easyList: "EasyList (ads)"
    case .easyPrivacy: "EasyPrivacy (trackers)"
    case .cookies: "EasyList Cookie List"
    }
  }
}

/// Downloads the filter lists into a folder and keeps the last good copy of
/// each. Asks the server only when the copy is older than `maxAge`, and
/// with If-None-Match / If-Modified-Since, so an unchanged list costs no
/// download.
public final class FilterListUpdater: @unchecked Sendable {
  public struct State: Codable, Equatable, Sendable {
    public var etag: String?
    public var lastModified: String?
    /// When the server was last asked.
    public var checked: Date
    /// When the text last changed.
    public var updated: Date
    /// A short hash of the text: compiled rules are kept by it.
    public var version: String
  }

  public let folder: URL
  public let maxAge: TimeInterval
  private let session: URLSession

  public init(folder: URL, maxAge: TimeInterval = 24 * 3600, session: URLSession = .shared) {
    self.folder = folder
    self.maxAge = maxAge
    self.session = session
    try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
  }

  public func text(_ list: FilterList) -> String? {
    try? String(contentsOf: textURL(list), encoding: .utf8)
  }

  public func state(_ list: FilterList) -> State? {
    (try? Data(contentsOf: stateURL(list))).flatMap { try? JSONDecoder().decode(State.self, from: $0) }
  }

  /// Fetches the list if it's due (or `force`). True when its text changed.
  /// A failed download keeps the last good copy and throws.
  @discardableResult
  public func update(_ list: FilterList, force: Bool = false, now: Date = Date()) async throws -> Bool {
    let old = state(list)
    if !force, let old, text(list) != nil, now.timeIntervalSince(old.checked) < maxAge { return false }
    var request = URLRequest(url: list.source)
    request.cachePolicy = .reloadIgnoringLocalCacheData
    if text(list) != nil {
      if let etag = old?.etag { request.setValue(etag, forHTTPHeaderField: "If-None-Match") }
      if let modified = old?.lastModified { request.setValue(modified, forHTTPHeaderField: "If-Modified-Since") }
    }
    let (data, response) = try await session.data(for: request)
    guard let http = response as? HTTPURLResponse else { throw URLError(.badServerResponse) }
    if http.statusCode == 304, var old {
      old.checked = now
      try save(old, list)
      return false
    }
    guard http.statusCode == 200, let body = String(data: data, encoding: .utf8),
      body.contains("[Adblock") || body.hasPrefix("!")
    else { throw URLError(.cannotParseResponse) }
    let version = Self.hash(body)
    let changed = version != old?.version
    try body.write(to: textURL(list), atomically: true, encoding: .utf8)
    try save(
      State(
        etag: http.value(forHTTPHeaderField: "ETag"),
        lastModified: http.value(forHTTPHeaderField: "Last-Modified"),
        checked: now, updated: changed ? now : (old?.updated ?? now), version: version), list)
    return changed
  }

  private func save(_ state: State, _ list: FilterList) throws {
    try JSONEncoder().encode(state).write(to: stateURL(list), options: .atomic)
  }

  private func textURL(_ list: FilterList) -> URL { folder.appendingPathComponent(list.rawValue + ".txt") }
  private func stateURL(_ list: FilterList) -> URL { folder.appendingPathComponent(list.rawValue + ".json") }

  /// FNV-1a, 64 bits, in hex: enough to tell versions apart.
  static func hash(_ text: String) -> String {
    var h: UInt64 = 0xcbf2_9ce4_8422_2325
    for byte in text.utf8 {
      h ^= UInt64(byte)
      h = h &* 0x100_0000_01b3
    }
    return String(h, radix: 16)
  }
}
