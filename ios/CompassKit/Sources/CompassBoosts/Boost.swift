// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Per-site CSS and JavaScript (docs/design/boosts.md), the desktop's
/// compass::boosts::Boost. Its JSON (Boosts.json, an array of these) has
/// the desktop's snake_case keys; the times are microseconds since the
/// Windows epoch, as sync carries them.
public struct Boost: Codable, Equatable, Sendable, Identifiable {
  /// Where the JavaScript runs: a world of its own, apart from the page's
  /// scripts (the default), or the page's own.
  public enum World: String, Codable, Sendable, CaseIterable {
    case isolated, main
  }

  /// Who made the Boost.
  public enum Author: String, Codable, Sendable {
    case user, claude
  }

  /// A lowercase UUID; also its sync client tag.
  public var id: String
  public var name: String
  /// A `BoostMatch` pattern, normalized.
  public var match: String
  /// Added as a <style> at document start, in every frame.
  public var css: String
  /// Run at document end, in every frame, in `jsWorld`.
  public var js: String
  public var jsWorld: World
  public var enabled: Bool
  public var author: Author
  public var createTime: Int64
  /// The last edit; of two edits, the later wins.
  public var updateTime: Int64

  public init(
    id: String = UUID().uuidString.lowercased(), name: String, match: String, css: String = "", js: String = "",
    jsWorld: World = .isolated, enabled: Bool = true, author: Author = .user, createTime: Int64 = 0,
    updateTime: Int64 = 0
  ) {
    self.id = id
    self.name = name
    self.match = match
    self.css = css
    self.js = js
    self.jsWorld = jsWorld
    self.enabled = enabled
    self.author = author
    self.createTime = createTime
    self.updateTime = updateTime
  }

  enum CodingKeys: String, CodingKey {
    case id, name, match, css, js, enabled, author
    case jsWorld = "js_world"
    case createTime = "create_time"
    case updateTime = "update_time"
  }

  /// Missing fields take their defaults; a world or author this version
  /// doesn't know is the safe one (isolated, user).
  public init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    id = try c.decode(String.self, forKey: .id)
    name = try c.decodeIfPresent(String.self, forKey: .name) ?? ""
    match = try c.decode(String.self, forKey: .match)
    css = try c.decodeIfPresent(String.self, forKey: .css) ?? ""
    js = try c.decodeIfPresent(String.self, forKey: .js) ?? ""
    jsWorld = (try c.decodeIfPresent(String.self, forKey: .jsWorld)).flatMap(World.init(rawValue:)) ?? .isolated
    enabled = try c.decodeIfPresent(Bool.self, forKey: .enabled) ?? true
    author = (try c.decodeIfPresent(String.self, forKey: .author)).flatMap(Author.init(rawValue:)) ?? .user
    createTime = try c.decodeIfPresent(Int64.self, forKey: .createTime) ?? 0
    updateTime = try c.decodeIfPresent(Int64.self, forKey: .updateTime) ?? createTime
  }

  /// The parsed `match`; nil for a pattern this version can't read.
  public var parsedMatch: BoostMatch? { BoostMatch(match) }

  /// What the size limit counts: the CSS and JavaScript, in UTF-8 bytes.
  public var codeBytes: Int { css.utf8.count + js.utf8.count }

  // MARK: Times

  /// Microseconds from 1601-01-01 to 1970-01-01 (base::Time's epoch).
  static let windowsEpochOffset: Int64 = 11_644_473_600 * 1_000_000

  public static func windowsMicros(_ date: Date) -> Int64 {
    Int64((date.timeIntervalSince1970 * 1_000_000).rounded(.down)) + windowsEpochOffset
  }

  public static func date(windowsMicros: Int64) -> Date {
    Date(timeIntervalSince1970: TimeInterval(windowsMicros - windowsEpochOffset) / 1_000_000)
  }

  public var created: Date { Self.date(windowsMicros: createTime) }
  public var updated: Date { Self.date(windowsMicros: updateTime) }
}

/// What save_boost (or the editor) asks for: nil fields keep the Boost's
/// (a new one's are empty, isolated and enabled).
public struct BoostDraft: Equatable, Sendable {
  public var id: String?
  public var name: String
  public var match: String
  public var css: String?
  public var js: String?
  /// "isolated" or "main".
  public var jsWorld: String?
  public var enabled: Bool?

  public init(
    id: String? = nil, name: String, match: String, css: String? = nil, js: String? = nil, jsWorld: String? = nil,
    enabled: Bool? = nil
  ) {
    self.id = id
    self.name = name
    self.match = match
    self.css = css
    self.js = js
    self.jsWorld = jsWorld
    self.enabled = enabled
  }
}

/// A profile's Boosts and the rules for changing them, as the desktop's
/// BoostStore keeps them (browser/boosts/boost_store.cc): at most
/// `maxBoosts`, each with at most `maxBytes` of CSS and JavaScript,
/// oldest first.
public struct BoostList: Equatable, Sendable {
  public static let maxBytes = 256 * 1024
  public static let maxBoosts = 200
  public static let maxNameChars = 100

  public private(set) var boosts: [Boost] = []

  public init(boosts: [Boost] = []) {
    var seen = Set<String>()
    self.boosts = boosts.filter { Self.isValid($0) && seen.insert($0.id).inserted }
    sort()
  }

  public enum Failure: Error, Equatable, CustomStringConvertible {
    case emptyName, nameTooLong, badMatch(String), badWorld(String), tooLarge, full, notFound(String)

    public var description: String {
      switch self {
      case .emptyName: "A Boost needs a name."
      case .nameTooLong: "A Boost's name is at most \(BoostList.maxNameChars) characters."
      case .badMatch(let match):
        "“\(match)” isn't a site pattern. Use a host (“example.com”, which also covers www.), “*.example.com” for the domain and all its subdomains, optionally followed by a path prefix (“github.com/settings”)."
      case .badWorld(let world): "js_world is “isolated” or “main”, not “\(world)”."
      case .tooLarge: "A Boost's CSS and JavaScript are at most \(BoostList.maxBytes / 1024) KB together."
      case .full: "There are already \(BoostList.maxBoosts) Boosts. Delete one first."
      case .notFound(let id): "No Boost has the id \(id)."
      }
    }
  }

  static func isValidID(_ id: String) -> Bool {
    id.count == 36 && id == id.lowercased() && UUID(uuidString: id) != nil
  }

  /// A Boost this version keeps: a UUID, a pattern in its normalized form,
  /// a name and code within the limits.
  public static func isValid(_ boost: Boost) -> Bool {
    isValidID(boost.id) && BoostMatch(boost.match)?.pattern == boost.match && boost.codeBytes <= maxBytes
      && boost.name.count <= maxNameChars
  }

  public func find(_ id: String) -> Boost? {
    let id = id.lowercased()
    return boosts.first { $0.id == id }
  }

  /// The enabled Boosts for a page.
  public func matching(_ url: URL) -> [Boost] {
    boosts.filter { $0.enabled && ($0.parsedMatch?.matches(url) ?? false) }
  }

  /// The Boosts that apply somewhere on `host` (enabled or not).
  public func forHost(_ host: String) -> [Boost] {
    boosts.filter { $0.parsedMatch?.matchesHost(host) ?? false }
  }

  /// What `draft` would make, not saved: a new Boost (a new id) or the
  /// one it edits, changed; unchanged (times included) when it changes
  /// nothing.
  public func resolve(_ draft: BoostDraft, author: Boost.Author, now: Date = Date()) -> Result<Boost, Failure> {
    let name = draft.name.trimmingCharacters(in: .whitespacesAndNewlines)
    if name.isEmpty { return .failure(.emptyName) }
    if name.count > Self.maxNameChars { return .failure(.nameTooLong) }
    guard let match = BoostMatch(draft.match) else { return .failure(.badMatch(draft.match)) }
    var world: Boost.World?
    if let raw = draft.jsWorld, !raw.isEmpty {
      guard let parsed = Boost.World(rawValue: raw.lowercased()) else { return .failure(.badWorld(raw)) }
      world = parsed
    }
    let stamp = Boost.windowsMicros(now)
    var boost: Boost
    let before: Boost?
    if let id = draft.id, !id.isEmpty {
      guard let existing = find(id) else { return .failure(.notFound(id)) }
      before = existing
      boost = existing
    } else {
      guard boosts.count < Self.maxBoosts else { return .failure(.full) }
      before = nil
      boost = Boost(name: name, match: match.pattern, author: author, createTime: stamp, updateTime: stamp)
    }
    boost.name = name
    boost.match = match.pattern
    if let css = draft.css { boost.css = css }
    if let js = draft.js { boost.js = js }
    if let world { boost.jsWorld = world }
    if let enabled = draft.enabled { boost.enabled = enabled }
    if boost.codeBytes > Self.maxBytes { return .failure(.tooLarge) }
    if let before, boost != before { boost.updateTime = max(stamp, before.updateTime + 1) }
    return .success(boost)
  }

  /// Whether saving `boost` over `before` needs the user's approval when
  /// Claude asks for it: it has JavaScript, and that JavaScript is new or
  /// changed, or runs elsewhere (another world or pattern) than before.
  public static func needsApproval(_ boost: Boost, before: Boost?) -> Bool {
    guard !boost.js.isEmpty else { return false }
    guard let before, !before.js.isEmpty else { return true }
    return before.js != boost.js || before.jsWorld != boost.jsWorld || before.match != boost.match
  }

  /// Whether Claude deleting `boost` needs approval: it has JavaScript.
  public static func deleteNeedsApproval(_ boost: Boost) -> Bool { !boost.js.isEmpty }

  /// Adds `boost` or replaces the one with its id; false if it isn't
  /// valid, or it's new and the list is full.
  @discardableResult
  public mutating func put(_ boost: Boost) -> Bool {
    guard Self.isValid(boost) else { return false }
    if let i = boosts.firstIndex(where: { $0.id == boost.id }) {
      boosts[i] = boost
    } else {
      guard boosts.count < Self.maxBoosts else { return false }
      boosts.append(boost)
      sort()
    }
    return true
  }

  @discardableResult
  public mutating func remove(_ id: String) -> Boost? {
    let id = id.lowercased()
    guard let i = boosts.firstIndex(where: { $0.id == id }) else { return nil }
    return boosts.remove(at: i)
  }

  /// Puts `boost` back as it was (Undo), newer than any synced copy.
  @discardableResult
  public mutating func restore(_ boost: Boost, now: Date = Date()) -> Bool {
    var restored = boost
    restored.updateTime = max(Boost.windowsMicros(now), boost.updateTime, (find(boost.id)?.updateTime ?? 0) + 1)
    return put(restored)
  }

  /// Sync: `upserts` replace or add Boosts by id unless the local one was
  /// updated later; `deleted` go. Whether anything changed.
  @discardableResult
  public mutating func applyRemote(_ upserts: [Boost], deleted: Set<String>) -> Bool {
    var changed = false
    for remote in upserts where Self.isValid(remote) {
      if let i = boosts.firstIndex(where: { $0.id == remote.id }) {
        if remote.updateTime >= boosts[i].updateTime && boosts[i] != remote {
          boosts[i] = remote
          changed = true
        }
      } else {
        // Not capped: one dropped here would be deleted from the account
        // by sync. The cap is for what's added here.
        boosts.append(remote)
        changed = true
      }
    }
    let before = boosts.count
    boosts.removeAll { deleted.contains($0.id) }
    changed = changed || boosts.count != before
    if changed { sort() }
    return changed
  }

  private mutating func sort() {
    // Stable: equal creation times keep their order.
    boosts = boosts.enumerated().sorted { a, b in
      a.element.createTime != b.element.createTime ? a.element.createTime < b.element.createTime : a.offset < b.offset
    }.map(\.element)
  }

  // MARK: Boosts.json

  public static func decode(_ data: Data) -> BoostList? {
    (try? JSONDecoder().decode([Boost].self, from: data)).map { BoostList(boosts: $0) }
  }

  public func encoded() -> Data {
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
    return (try? encoder.encode(boosts)) ?? Data("[]".utf8)
  }
}
