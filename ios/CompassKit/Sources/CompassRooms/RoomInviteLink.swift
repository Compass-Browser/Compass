// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation

/// What a room shares.
public enum RoomKind: String, Sendable, CaseIterable {
  case space, folder, easel
}

/// An invite link: everything a new member needs to join a room, in the
/// fragment of https://compassbrowser.org/join#… (or compass://join#…), which
/// never reaches a server. The desktop's twin is
/// components/sync/rooms/room_invite_link.cc; see docs/design/sync.md,
/// "Rooms: crypto and links", and common/rooms/vectors.json.
public struct RoomInviteLink: Sendable, Equatable {
  public static let prefix = "https://compassbrowser.org/join#"
  public static let deepLinkPrefix = "compass://join#"
  public static let maxLinkLength = 4096
  public static let maxInviteIDLength = 64
  /// UTF-8 bytes.
  public static let maxNameLength = 256

  /// The room's server: https, or http on localhost, 127.0.0.1 or [::1]. No
  /// user info, query or fragment.
  public var serverURL: String
  public var roomID: Data
  /// The server's ID for the invite: 1 to 64 of [A-Za-z0-9_-].
  public var inviteID: String
  /// Proves the invite to the server, which keeps
  /// `RoomCrypto.inviteSecretHash` of it.
  public var inviteSecret: Data
  /// The room key of `epoch`, so the joiner reads at once.
  public var epochKey: Data
  public var epoch: UInt32
  /// The shared item's name, for the join preview.
  public var name: String
  public var kind: RoomKind

  public init(
    serverURL: String, roomID: Data, inviteID: String, inviteSecret: Data, epochKey: Data,
    epoch: UInt32, name: String, kind: RoomKind
  ) {
    self.serverURL = serverURL
    self.roomID = roomID
    self.inviteID = inviteID
    self.inviteSecret = inviteSecret
    self.epochKey = epochKey
    self.epoch = epoch
    self.name = name
    self.kind = kind
  }

  /// Compact JSON with sorted keys, strings escaping only '"', '\' and
  /// control characters, base64url without padding; nil if a rule above is
  /// broken.
  public var json: String? {
    guard isValid else { return nil }
    return "{\"e\":\(epoch),\"i\":\(Self.quote(inviteID)),\"k\":\(Self.quote(Self.b64(epochKey))),"
      + "\"n\":\(Self.quote(name)),\"r\":\(Self.quote(Self.b64(roomID))),\"s\":\(Self.quote(serverURL)),"
      + "\"t\":\(Self.quote(kind.rawValue)),\"x\":\(Self.quote(Self.b64(inviteSecret)))}"
  }

  /// `prefix` + base64url(json), or nil.
  public var link: String? {
    guard let json else { return nil }
    let link = Self.prefix + Self.b64(Data(json.utf8))
    return link.utf8.count <= Self.maxLinkLength ? link : nil
  }

  /// The invite in an https://compassbrowser.org/join# or compass://join#
  /// link, or nil if it's malformed or breaks a rule. Unknown JSON keys are
  /// ignored, for later versions.
  public init?(link text: String) {
    guard text.utf8.count <= Self.maxLinkLength else { return nil }
    let fragment: Substring
    if text.hasPrefix(Self.prefix) {
      fragment = text.dropFirst(Self.prefix.count)
    } else if text.hasPrefix(Self.deepLinkPrefix) {
      fragment = text.dropFirst(Self.deepLinkPrefix.count)
    } else {
      return nil
    }
    guard let data = Self.unb64(String(fragment)), !data.isEmpty,
      let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
      let serverURL = object["s"] as? String,
      let inviteID = object["i"] as? String,
      let name = object["n"] as? String,
      let kindName = object["t"] as? String, let kind = RoomKind(rawValue: kindName),
      let roomID = Self.unb64(object["r"], count: RoomCrypto.roomIDLength),
      let secret = Self.unb64(object["x"], count: RoomCrypto.inviteSecretLength),
      let key = Self.unb64(object["k"], count: RoomCrypto.roomKeyLength),
      let epoch = Self.epoch(object["e"])
    else { return nil }
    self.init(
      serverURL: serverURL, roomID: roomID, inviteID: inviteID, inviteSecret: secret,
      epochKey: key, epoch: epoch, name: name, kind: kind)
    guard isValid else { return nil }
  }

  // MARK: Rules

  var isValid: Bool {
    roomID.count == RoomCrypto.roomIDLength && inviteSecret.count == RoomCrypto.inviteSecretLength
      && epochKey.count == RoomCrypto.roomKeyLength && name.utf8.count <= Self.maxNameLength
      && Self.isValidInviteID(inviteID) && Self.isValidServerURL(serverURL)
  }

  static func isValidInviteID(_ id: String) -> Bool {
    let bytes = Array(id.utf8)
    return !bytes.isEmpty && bytes.count <= maxInviteIDLength
      && bytes.allSatisfy { isAlphanumeric($0) || $0 == UInt8(ascii: "_") || $0 == UInt8(ascii: "-") }
  }

  /// The host and port are read from the text itself, the same way on every
  /// client, before URLComponents has its say.
  static func isValidServerURL(_ text: String) -> Bool {
    let https = text.hasPrefix("https://")
    guard https || text.hasPrefix("http://") else { return false }
    let allowed = Set("-._~:/[]!$&'()*+,;=%".utf8)
    guard text.utf8.allSatisfy({ isAlphanumeric($0) || allowed.contains($0) }) else { return false }
    let rest = Array(text.utf8.dropFirst(https ? 8 : 7))
    let authority = Array(rest.prefix { $0 != UInt8(ascii: "/") })
    var host = authority
    var port: [UInt8] = []
    if authority.first == UInt8(ascii: "[") {
      guard let end = authority.firstIndex(of: UInt8(ascii: "]")) else { return false }
      host = Array(authority[...end])
      port = Array(authority[(end + 1)...])
    } else if let colon = authority.firstIndex(of: UInt8(ascii: ":")) {
      host = Array(authority[..<colon])
      port = Array(authority[colon...])
    }
    guard !host.isEmpty else { return false }
    if !port.isEmpty {
      let digits = port.dropFirst()
      guard port.first == UInt8(ascii: ":"), !digits.isEmpty, digits.count <= 5,
        digits.allSatisfy({ $0 >= UInt8(ascii: "0") && $0 <= UInt8(ascii: "9") }),
        let value = Int(String(decoding: digits, as: UTF8.self)), value >= 1, value <= 65535
      else { return false }
    }
    let hostName = String(decoding: host, as: UTF8.self)
    if !https && !["localhost", "127.0.0.1", "[::1]"].contains(hostName) { return false }
    guard let components = URLComponents(string: text), let parsed = components.host,
      !parsed.isEmpty, components.user == nil, components.password == nil,
      components.query == nil, components.fragment == nil
    else { return false }
    return true
  }

  static func isAlphanumeric(_ b: UInt8) -> Bool {
    (b >= UInt8(ascii: "a") && b <= UInt8(ascii: "z")) || (b >= UInt8(ascii: "A") && b <= UInt8(ascii: "Z"))
      || (b >= UInt8(ascii: "0") && b <= UInt8(ascii: "9"))
  }

  /// A JSON number that is a whole UInt32 (not a boolean).
  static func epoch(_ value: Any?) -> UInt32? {
    guard let number = value as? NSNumber, CFGetTypeID(number) != CFBooleanGetTypeID() else {
      return nil
    }
    let double = number.doubleValue
    guard double >= 0, double <= Double(UInt32.max), double.rounded(.down) == double else {
      return nil
    }
    return UInt32(double)
  }

  // MARK: Encoding

  static func quote(_ string: String) -> String {
    var out = Data("\"".utf8)
    for byte in string.utf8 {
      switch byte {
      case 0x22: out += Data("\\\"".utf8)
      case 0x5c: out += Data("\\\\".utf8)
      case 0x08: out += Data("\\b".utf8)
      case 0x0c: out += Data("\\f".utf8)
      case 0x0a: out += Data("\\n".utf8)
      case 0x0d: out += Data("\\r".utf8)
      case 0x09: out += Data("\\t".utf8)
      case ..<0x20: out += Data(String(format: "\\u%04x", byte).utf8)
      default: out.append(byte)
      }
    }
    out += Data("\"".utf8)
    return String(decoding: out, as: UTF8.self)
  }

  static func b64(_ data: Data) -> String { data.base64URLEncodedString(padded: false) }

  /// Canonical unpadded base64url only.
  static func unb64(_ text: String) -> Data? {
    let alphabet = Set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_".utf8)
    guard text.utf8.allSatisfy({ alphabet.contains($0) }), text.utf8.count % 4 != 1 else {
      return nil
    }
    var standard = text.replacingOccurrences(of: "-", with: "+")
      .replacingOccurrences(of: "_", with: "/")
    standard += String(repeating: "=", count: (4 - standard.utf8.count % 4) % 4)
    guard let data = Data(base64Encoded: standard), b64(data) == text else { return nil }
    return data
  }

  static func unb64(_ value: Any?, count: Int) -> Data? {
    guard let text = value as? String, let data = unb64(text), data.count == count else {
      return nil
    }
    return data
  }
}
