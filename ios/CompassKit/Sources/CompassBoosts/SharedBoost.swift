// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

#if canImport(Compression)
  import Compression
#endif

/// A Boost as it's shared (docs/design/boosts.md, "Sharing"): its name,
/// site, CSS and JavaScript, without its id, author, times or switch.
///
/// - JSON (UTF-8): `{"v":1,"name":…,"match":…,"css":…,"js":…,"js_world":…}`;
///   css, js and js_world may be absent, unknown keys are ignored, and a
///   `v` other than 1 is refused.
/// - Link: `https://compassbrowser.org/boost#` + base64url (no padding) of
///   the raw DEFLATE of that JSON.
/// - File: `<name>.compassboost`, the JSON pretty-printed.
///
/// Adding one always goes through `BoostList.resolveShared`, the rules a
/// save follows, as a new Boost by "shared".
public struct SharedBoost: Hashable, Sendable {
  public var name: String
  public var match: String
  public var css: String
  public var js: String
  public var jsWorld: Boost.World

  public static let version = 1
  /// Where share links point; the fragment carries the Boost.
  public static let linkPrefix = "https://compassbrowser.org/boost#"
  /// A link longer than this isn't offered; the file is.
  public static let maxLinkBytes = 64 * 1024
  /// The most JSON a link or file may hold (after decompression): a guard
  /// against decompression bombs, above what the code limit allows.
  public static let maxJSONBytes = 512 * 1024
  public static let fileExtension = "compassboost"
  public static let mimeType = "application/vnd.compassbrowser.boost+json"
  public static let typeIdentifier = "org.compassbrowser.boost"

  public init(name: String, match: String, css: String = "", js: String = "", jsWorld: Boost.World = .isolated) {
    self.name = name
    self.match = match
    self.css = css
    self.js = js
    self.jsWorld = jsWorld
  }

  public init(_ boost: Boost) {
    self.init(name: boost.name, match: boost.match, css: boost.css, js: boost.js, jsWorld: boost.jsWorld)
  }

  public enum Failure: Error, Equatable, CustomStringConvertible {
    /// Not base64url, or not raw DEFLATE.
    case unreadable
    /// More than `maxJSONBytes` (compressed or not).
    case tooLarge
    /// Not a JSON object with a version, a name and a match.
    case notABoost
    case newerVersion
    /// Neither CSS nor JavaScript.
    case empty
    /// What a save would refuse.
    case invalid(BoostList.Failure)

    public var description: String {
      switch self {
      case .unreadable:
        String(localized: "This link or file doesn't hold a Boost. It may have been cut short when it was copied.", bundle: .module, comment: "Adding a shared Boost: error.")
      case .tooLarge: String(localized: "This Boost is too large to add.", bundle: .module, comment: "Adding a shared Boost: error.")
      case .notABoost: String(localized: "This link or file doesn't hold a Boost.", bundle: .module, comment: "Adding a shared Boost: error.")
      case .newerVersion: String(localized: "This Boost was made by a newer Compass. Update Compass to add it.", bundle: .module, comment: "Adding a shared Boost: error.")
      case .empty: String(localized: "This Boost has no CSS or JavaScript.", bundle: .module, comment: "Adding a shared Boost: error.")
      case .invalid(let failure): failure.description
      }
    }
  }

  // MARK: JSON

  /// The JSON, compact (links) or pretty-printed (files), keys in the
  /// format's order; css, js and js_world only when there's code for them.
  public func json(pretty: Bool = false) -> Data {
    var fields: [(String, String)] = [("v", String(Self.version)), ("name", Self.quote(name)), ("match", Self.quote(match))]
    if !css.isEmpty { fields.append(("css", Self.quote(css))) }
    if !js.isEmpty {
      fields.append(("js", Self.quote(js)))
      fields.append(("js_world", Self.quote(jsWorld.rawValue)))
    }
    let text =
      pretty
      ? "{\n" + fields.map { "  \"\($0.0)\": \($0.1)" }.joined(separator: ",\n") + "\n}\n"
      : "{" + fields.map { "\"\($0.0)\":\($0.1)" }.joined(separator: ",") + "}"
    return Data(text.utf8)
  }

  /// A JSON string literal.
  private static func quote(_ string: String) -> String {
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.withoutEscapingSlashes]
    return (try? encoder.encode(string)).flatMap { String(data: $0, encoding: .utf8) } ?? "\"\""
  }

  /// Reads the JSON of a link or a file. Only checks the format; what a
  /// save would refuse is `BoostList.resolveShared`'s.
  public static func decode(json data: Data) -> Result<SharedBoost, Failure> {
    guard data.count <= maxJSONBytes else { return .failure(.tooLarge) }
    guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
      return .failure(.notABoost)
    }
    // A number (not a boolean) that is exactly 1.
    guard let v = object["v"] as? NSNumber, CFGetTypeID(v) != CFBooleanGetTypeID() else {
      return .failure(.notABoost)
    }
    guard v.doubleValue == Double(version) else { return .failure(.newerVersion) }
    // The strings: match is needed; the others may be absent (or null).
    var strings: [String: String] = [:]
    for key in ["name", "match", "css", "js", "js_world"] {
      guard let value = object[key], !(value is NSNull) else { continue }
      guard let string = value as? String else { return .failure(.notABoost) }
      strings[key] = string
    }
    guard let match = strings["match"] else { return .failure(.notABoost) }
    let name = strings["name"] ?? "", css = strings["css"] ?? "", js = strings["js"] ?? ""
    var world = Boost.World.isolated
    if let worldName = strings["js_world"] {
      guard let parsed = Boost.World(rawValue: worldName) else {
        return .failure(.invalid(.badWorld(worldName)))
      }
      world = parsed
    }
    return .success(SharedBoost(name: name, match: match, css: css, js: js, jsWorld: world))
  }

  // MARK: Links

  /// The share link; nil if it can't be made (no compression here).
  public func link() -> URL? {
    guard let deflated = Self.deflate(json()) else { return nil }
    return URL(string: Self.linkPrefix + Self.base64URL(deflated))
  }

  /// Reads a link's fragment (what follows `#`).
  public static func decode(fragment: String) -> Result<SharedBoost, Failure> {
    // Base64 of maxJSONBytes would be longer than this, compressed or not.
    guard fragment.utf8.count <= (maxJSONBytes / 3 + 1) * 4 else { return .failure(.tooLarge) }
    guard let deflated = fromBase64URL(fragment) else { return .failure(.unreadable) }
    switch inflate(deflated, limit: maxJSONBytes) {
    case .failure(let failure): return .failure(failure)
    case .success(let json): return decode(json: json)
    }
  }

  /// RFC 4648 §5, without padding.
  static func base64URL(_ data: Data) -> String {
    data.base64EncodedString().replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_")
      .replacingOccurrences(of: "=", with: "")
  }

  /// Padding is optional; `+`, `/`, whitespace and other characters are
  /// refused.
  static func fromBase64URL(_ string: String) -> Data? {
    var text = string
    while text.hasSuffix("=") { text.removeLast() }
    guard !text.isEmpty, text.count % 4 != 1,
      text.unicodeScalars.allSatisfy({
        ("A"..."Z").contains($0) || ("a"..."z").contains($0) || ("0"..."9").contains($0) || $0 == "-" || $0 == "_"
      })
    else { return nil }
    text = text.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
    text += String(repeating: "=", count: (4 - text.count % 4) % 4)
    return Data(base64Encoded: text)
  }

  // MARK: Files

  /// `<name>.compassboost`, the name made safe for a file name.
  public var fileName: String {
    let unsafe = CharacterSet(charactersIn: "/\\:?%*|\"<>").union(.controlCharacters).union(.newlines)
    var base = String(String.UnicodeScalarView(name.unicodeScalars.map { unsafe.contains($0) ? "-" : $0 }))
      .trimmingCharacters(in: .whitespaces)
    while base.hasPrefix(".") { base.removeFirst() }
    if base.isEmpty { base = "Boost" }
    return String(base.prefix(100)) + "." + Self.fileExtension
  }

  /// The file's contents.
  public var fileData: Data { json(pretty: true) }

  /// Reads a .compassboost file.
  public static func decode(file data: Data) -> Result<SharedBoost, Failure> { decode(json: data) }

  // MARK: Raw DEFLATE (RFC 1951)

  /// Compression's ZLIB is raw DEFLATE, without zlib's or gzip's header.
  static func deflate(_ data: Data) -> Data? {
    #if canImport(Compression)
      if data.isEmpty { return nil }
      return process(data, operation: COMPRESSION_STREAM_ENCODE, limit: Int.max).flatMap { try? $0.get() }
    #else
      return nil
    #endif
  }

  /// Inflates at most `limit` bytes: past that it stops and refuses, so a
  /// small link can't make it inflate a huge one.
  static func inflate(_ data: Data, limit: Int) -> Result<Data, Failure> {
    #if canImport(Compression)
      return process(data, operation: COMPRESSION_STREAM_DECODE, limit: limit) ?? .failure(.unreadable)
    #else
      return .failure(.unreadable)
    #endif
  }

  #if canImport(Compression)
    /// Streams `data` through Compression in 64 KB steps. nil when the
    /// input isn't valid (or ends early); `.tooLarge` past `limit`.
    private static func process(_ data: Data, operation: compression_stream_operation, limit: Int) -> Result<
      Data, Failure
    >? {
      let stream = UnsafeMutablePointer<compression_stream>.allocate(capacity: 1)
      defer { stream.deallocate() }
      guard compression_stream_init(stream, operation, COMPRESSION_ZLIB) == COMPRESSION_STATUS_OK else { return nil }
      defer { compression_stream_destroy(stream) }
      let chunk = 64 * 1024
      let buffer = UnsafeMutablePointer<UInt8>.allocate(capacity: chunk)
      defer { buffer.deallocate() }
      var output = Data()
      return data.withUnsafeBytes { (input: UnsafeRawBufferPointer) -> Result<Data, Failure>? in
        guard let base = input.bindMemory(to: UInt8.self).baseAddress else { return nil }
        stream.pointee.src_ptr = base
        stream.pointee.src_size = input.count
        while true {
          stream.pointee.dst_ptr = buffer
          stream.pointee.dst_size = chunk
          let before = stream.pointee.src_size
          let status = compression_stream_process(stream, Int32(COMPRESSION_STREAM_FINALIZE.rawValue))
          let produced = chunk - stream.pointee.dst_size
          if produced > 0 {
            guard output.count + produced <= limit else { return .failure(.tooLarge) }
            output.append(buffer, count: produced)
          }
          switch status {
          case COMPRESSION_STATUS_END:
            return .success(output)
          case COMPRESSION_STATUS_OK:
            // No progress with all the input given: it ends too early.
            if produced == 0 && stream.pointee.src_size == before { return nil }
          default:
            return nil
          }
        }
      }
    }
  #endif
}

extension BoostList {
  /// What adding a shared Boost would make, not saved: always a new Boost
  /// (a new id), enabled, by "shared", under the rules a save follows
  /// (the name, the site pattern, the code's size, the number of Boosts).
  /// An empty name becomes the site, as in the editor.
  public func resolveShared(_ shared: SharedBoost, now: Date = Date()) -> Result<Boost, SharedBoost.Failure> {
    if shared.css.isEmpty && shared.js.isEmpty { return .failure(.empty) }
    let name = shared.name.trimmingCharacters(in: .whitespacesAndNewlines)
    let draft = BoostDraft(
      name: name.isEmpty ? shared.match : shared.name, match: shared.match, css: shared.css, js: shared.js,
      jsWorld: shared.jsWorld.rawValue, enabled: true)
    return resolve(draft, author: .shared, now: now).mapError { .invalid($0) }
  }
}
