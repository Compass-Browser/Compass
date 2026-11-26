// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CryptoKit
import Foundation
// The app uses the boards as the sync messages, and links CompassSync
// rather than SyncProto.
@_exported import SyncProto

/// A board, an object on it, and an image (docs/design/easels.md): the sync
/// messages themselves, so fields and content kinds this version doesn't
/// know are kept and passed on.
public typealias EaselBoard = SyncPb_CompassEasel
public typealias EaselObject = SyncPb_CompassEaselObject
public typealias EaselAsset = SyncPb_CompassEaselAsset

/// The desktop's limits (browser/easels/easel_store.h).
public enum EaselLimits {
  public static let maxEasels = 200
  public static let maxObjectsPerEasel = 1000
  public static let maxTextChars = 20000
  public static let maxFreehandPoints = 4000
  public static let maxTitleChars = 200
  public static let maxAssetBytes = 512 * 1024
  public static let maxAssetPixels = 2048
  public static let maxAssetBytesTotal = 64 * 1024 * 1024
  /// An image no object points at goes after this long.
  public static let unusedAssetLifetime: TimeInterval = 30 * 24 * 60 * 60
}

/// CompassEaselShape.kind.
public enum EaselShapeKind: Int32, Sendable {
  case rectangle = 1
  case circle = 2
  case arrow = 3
  case freehand = 4
}

/// What went wrong, in words for the user (the desktop's).
public struct EaselError: Error, Equatable, Sendable, CustomStringConvertible {
  public let message: String
  public init(_ message: String) { self.message = message }
  public var description: String { message }
}

/// Board URLs: a board's tab is "compass://easel/<uuid>", and
/// "compass://easel/" is the list of boards.
public enum EaselURL {
  public static let list = "compass://easel/"
  static let prefixes = ["compass://easel/", "chrome://easel/"]

  public static func url(_ uuid: String) -> String { prefixes[0] + uuid.lowercased() }

  /// The board of a board URL (compass:// or chrome://, any case, a query
  /// or fragment ignored), or nil for any other URL.
  public static func uuid(from url: String) -> String? {
    for prefix in prefixes where url.lowercased().hasPrefix(prefix) {
      var rest = url.dropFirst(prefix.count).lowercased()
      if let end = rest.firstIndex(where: { "?#/".contains($0) }) { rest = String(rest[..<end]) }
      return Easels.isLowerUUID(rest) ? rest : nil
    }
    return nil
  }

  /// The Easels list ("compass://easel/", with or without its slash).
  public static func isList(_ url: String) -> Bool {
    let lower = url.lowercased()
    return prefixes.contains { lower == $0 || lower == String($0.dropLast()) }
  }

  /// A board or the list: shown by the app, not a web view.
  public static func isEasel(_ url: String) -> Bool { isList(url) || uuid(from: url) != nil }
}

public enum Easels {
  /// SHA-256 as lowercase hex.
  public static func sha256Hex(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
  }

  /// CompassEaselAsset.uuid for an image with this SHA-256: the first 16
  /// bytes of SHA-256("compass-easel-asset:" + sha256), version 5 and
  /// variant bits set, lowercase 8-4-4-4-12.
  public static func assetUUID(sha256: String) -> String {
    var b = Array(SHA256.hash(data: Data(("compass-easel-asset:" + sha256).utf8)).prefix(16))
    b[6] = (b[6] & 0x0f) | 0x50
    b[8] = (b[8] & 0x3f) | 0x80
    let hex = b.map { String(format: "%02x", $0) }.joined()
    let c = Array(hex)
    return [c[0..<8], c[8..<12], c[12..<16], c[16..<20], c[20..<32]].map { String($0) }.joined(separator: "-")
  }

  /// Microseconds since the Windows epoch, as the sync messages keep times.
  static let windowsEpochOffset: Int64 = 11_644_473_600 * 1_000_000
  public static func syncTime(_ date: Date) -> Int64 {
    Int64((date.timeIntervalSince1970 * 1_000_000).rounded(.down)) + windowsEpochOffset
  }
  public static func date(syncTime: Int64) -> Date {
    Date(timeIntervalSince1970: TimeInterval(syncTime - windowsEpochOffset) / 1_000_000)
  }

  static func isLowerUUID(_ s: some StringProtocol) -> Bool {
    s.count == 36 && s == s.lowercased() && UUID(uuidString: String(s)) != nil
  }

  static func isSha256Hex(_ s: String) -> Bool {
    s.utf8.count == 64 && s.utf8.allSatisfy { ($0 >= 48 && $0 <= 57) || ($0 >= 97 && $0 <= 102) }
  }

  static func isKnownMime(_ mime: String) -> Bool {
    mime == "image/png" || mime == "image/jpeg" || mime == "image/webp"
  }

  /// At most `max` UTF-16 units (the desktop counts them so), not ending
  /// in half a surrogate pair.
  static func truncate(_ text: String, _ max: Int) -> String {
    guard text.utf16.count > max else { return text }
    var units = Array(text.utf16.prefix(max))
    if let last = units.last, UTF16.isLeadSurrogate(last) { units.removeLast() }
    return String(decoding: units, as: UTF16.self)
  }

  static func trimmedTitle(_ title: String) -> String {
    truncate(title.trimmingCharacters(in: .whitespacesAndNewlines), EaselLimits.maxTitleChars)
  }

  /// Keeps evenly spaced points of a stroke (x, y pairs) so it has at most
  /// `max`, both ends kept.
  static func simplify(_ points: [Double], max: Int) -> [Double] {
    let count = points.count / 2
    guard count > max else { return points }
    let step = Double(count - 1) / Double(max - 1)
    var kept: [Double] = []
    kept.reserveCapacity(max * 2)
    for i in 0..<max {
      let index = Swift.min(count - 1, Int((Double(i) * step).rounded()))
      kept.append(points[index * 2])
      kept.append(points[index * 2 + 1])
    }
    return kept
  }

  static func contentKinds(_ o: EaselObject) -> Int {
    [o.hasText, o.hasShape, o.hasImage, o.hasLink, o.hasCapture].filter { $0 }.count
  }

  /// The images an object points at.
  public static func assets(of o: EaselObject) -> Set<String> {
    var out: Set<String> = []
    if o.hasImage { out.insert(o.image.asset) }
    if o.hasLink, o.link.hasImageAsset { out.insert(o.link.imageAsset) }
    if o.hasCapture {
      out.insert(o.capture.imageAsset)
      if o.capture.hasFaviconAsset { out.insert(o.capture.faviconAsset) }
    }
    return out
  }

  /// Checks and normalizes an object a user (or an import) saves: the box,
  /// the content (exactly one kind, the limits, a freehand stroke
  /// simplified to the limit). The desktop's NormalizeObject().
  public static func normalize(_ input: EaselObject) -> Result<EaselObject, EaselError> {
    var o = input
    o.uuid = o.uuid.lowercased()
    o.easelUuid = o.easelUuid.lowercased()
    guard isLowerUUID(o.easelUuid) else { return .failure(EaselError("The object has no board.")) }
    for value in [o.x, o.y, o.width, o.height, o.z] where !value.isFinite || abs(value) > 1e7 {
      return .failure(EaselError("The object's position is out of range."))
    }
    if o.width < 0 || o.height < 0 { return .failure(EaselError("The object's size can't be negative.")) }
    let kinds = contentKinds(o)
    if kinds != 1 {
      // One this version doesn't know is kept as it is (from sync); a
      // user's must have one.
      if kinds == 0 && !o.unknownFields.data.isEmpty { return .success(o) }
      return .failure(EaselError("An object has exactly one kind of content."))
    }
    if o.hasText { o.text.text = truncate(o.text.text, EaselLimits.maxTextChars) }
    if o.hasShape {
      var shape = o.shape
      guard shape.points.allSatisfy(\.isFinite) else { return .failure(EaselError("A shape's points must be numbers.")) }
      if shape.points.count % 2 == 1 { shape.points.removeLast() }
      if shape.kind == EaselShapeKind.arrow.rawValue && shape.points.count != 4 {
        return .failure(EaselError("An arrow has a start and an end."))
      }
      if shape.kind == EaselShapeKind.freehand.rawValue {
        if shape.points.count < 2 { return .failure(EaselError("A stroke needs points.")) }
        shape.points = simplify(shape.points, max: EaselLimits.maxFreehandPoints)
      }
      if shape.hasStrokeWidth && (!shape.strokeWidth.isFinite || shape.strokeWidth < 0 || shape.strokeWidth > 200) {
        shape.clearStrokeWidth()
      }
      o.shape = shape
    }
    if o.hasImage && !isSha256Hex(o.image.asset) { return .failure(EaselError(String(localized: "The image is missing.", bundle: .module, comment: "Easel error, shown in an alert or toast."))) }
    if o.hasCapture && !isSha256Hex(o.capture.imageAsset) {
      return .failure(EaselError(String(localized: "The capture's image is missing.", bundle: .module, comment: "Easel error, shown in an alert or toast. A capture is a live snapshot of part of a web page.")))
    }
    return .success(o)
  }
}
