// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Where a web extension comes from, and how its package is fetched: the
/// Chrome Web Store (a CRX, whose ZIP follows a signed header) or Firefox
/// Add-ons (an XPI, a plain ZIP), then unpacked by ExtensionPackage
/// (docs/design/ios.md, "Web extensions").
enum ExtensionSource: Codable, Hashable, Sendable {
  /// A Chrome Web Store ID: 32 letters a–p.
  case chromeWebStore(String)
  /// A Firefox Add-ons slug or numeric ID.
  case firefoxAddons(String)
  /// A file the user picked.
  case file

  /// The source a store page, or a pasted ID, names; nil for anything else.
  ///   chromewebstore.google.com/detail/<slug>/<id>
  ///   chrome.google.com/webstore/detail/<slug>/<id>
  ///   addons.mozilla.org/<locale>/firefox/addon/<slug>/
  init?(_ text: String) {
    let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    if Self.isChromeID(trimmed) {
      self = .chromeWebStore(trimmed)
      return
    }
    let withScheme = trimmed.contains("://") ? trimmed : "https://" + trimmed
    guard let url = URL(string: withScheme), let host = url.host()?.lowercased() else { return nil }
    let parts = url.pathComponents.filter { $0 != "/" }
    switch host {
    case "chromewebstore.google.com", "chrome.google.com":
      guard let id = parts.first(where: Self.isChromeID) else { return nil }
      self = .chromeWebStore(id)
    case "addons.mozilla.org":
      guard let index = parts.firstIndex(of: "addon"), index + 1 < parts.count else { return nil }
      self = .firefoxAddons(parts[index + 1])
    default:
      return nil
    }
  }

  static func isChromeID(_ s: String) -> Bool {
    s.count == 32 && s.allSatisfy { ("a"..."p").contains($0) }
  }

  /// The ID the extension keeps for its life here: its Chrome ID (some
  /// extensions name their own chrome-extension:// URLs), else a new UUID.
  /// WebKit uses it as the host of the extension's pages, and its storage
  /// is keyed by it.
  var preferredIdentifier: String {
    if case .chromeWebStore(let id) = self { return id }
    return UUID().uuidString.lowercased()
  }

  var storeName: String {
    switch self {
    case .chromeWebStore: "Chrome Web Store"
    case .firefoxAddons: "Firefox Add-ons"
    case .file: "File"
    }
  }

  /// The extension's page in its store.
  var pageURL: URL? {
    switch self {
    case .chromeWebStore(let id): URL(string: "https://chromewebstore.google.com/detail/\(id)")
    case .firefoxAddons(let slug): URL(string: "https://addons.mozilla.org/firefox/addon/\(slug)/")
    case .file: nil
    }
  }

  /// Downloads the package and returns its ZIP.
  func fetchZip() async throws -> Data {
    switch self {
    case .chromeWebStore(let id):
      // The update service Chrome itself installs from. It wants a recent
      // Chrome version, or it offers nothing for MV3 extensions.
      var components = URLComponents(string: "https://clients2.google.com/service/update2/crx")!
      components.queryItems = [
        URLQueryItem(name: "response", value: "redirect"),
        URLQueryItem(name: "prodversion", value: "142.0.0.0"),
        URLQueryItem(name: "acceptformat", value: "crx2,crx3"),
        URLQueryItem(name: "x", value: "id=\(id)&uc"),
      ]
      let data = try await Self.get(components.url!)
      return try Self.zip(fromCRX: data)
    case .firefoxAddons(let slug):
      let api = URL(string: "https://addons.mozilla.org/api/v5/addons/addon/\(slug)/")!
      struct Addon: Decodable {
        struct Version: Decodable {
          struct File: Decodable { let url: URL }
          let file: File
        }
        let current_version: Version
      }
      let addon = try JSONDecoder().decode(Addon.self, from: try await Self.get(api))
      return try Self.zip(fromCRX: try await Self.get(addon.current_version.file.url))
    case .file:
      throw ExtensionError.notDownloadable
    }
  }

  private static func get(_ url: URL) async throws -> Data {
    let (data, response) = try await URLSession.shared.data(from: url)
    guard let http = response as? HTTPURLResponse, http.statusCode == 200, !data.isEmpty else {
      throw ExtensionError.notFound
    }
    return data
  }

  /// A CRX's ZIP: version 3 is "Cr24", the version, the header's length and
  /// the header; version 2 is "Cr24", the version, the key's and the
  /// signature's lengths, then both. A ZIP is returned as it is.
  static func zip(fromCRX data: Data) throws -> Data {
    let bytes = [UInt8](data.prefix(16))
    if bytes.starts(with: [0x50, 0x4B, 0x03, 0x04]) { return data }
    guard bytes.count >= 12, bytes.starts(with: Array("Cr24".utf8)) else { throw ExtensionError.badPackage }
    func le32(_ at: Int) -> Int {
      Int(bytes[at]) | Int(bytes[at + 1]) << 8 | Int(bytes[at + 2]) << 16 | Int(bytes[at + 3]) << 24
    }
    let offset: Int
    switch le32(4) {
    case 3: offset = 12 + le32(8)
    case 2 where bytes.count >= 16: offset = 16 + le32(8) + le32(12)
    default: throw ExtensionError.badPackage
    }
    guard offset < data.count else { throw ExtensionError.badPackage }
    let zip = data.subdata(in: data.startIndex + offset..<data.endIndex)
    guard zip.starts(with: [0x50, 0x4B, 0x03, 0x04]) else { throw ExtensionError.badPackage }
    return zip
  }
}

enum ExtensionError: LocalizedError {
  case notFound, badPackage, notDownloadable, unsupported

  var errorDescription: String? {
    switch self {
    case .notFound: "The store didn't send the extension. Check the link."
    case .badPackage: "That isn't an extension package (.crx, .xpi or .zip)."
    case .notDownloadable: "This extension came from a file; add it again from the file to update it."
    case .unsupported: "Extensions need iOS 18.4 or later."
    }
  }
}
