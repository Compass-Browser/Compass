// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Compression
import Foundation

/// An extension's files on disk: its ZIP unpacked, and its manifest made
/// runnable here.
///
/// WKWebView runs service workers only in apps with the default-browser
/// entitlement (or App-Bound Domains), so a Chrome extension's background
/// service worker never starts in Compass. WebKit runs a background page
/// as well, though, and the APIs a service worker uses are the same there,
/// so `background.service_worker` becomes `background.scripts` (an ES
/// module stays one). Files a classic worker loads with importScripts()
/// (literal names) are listed before it, and importScripts() becomes a
/// no-op (docs/design/ios.md, "Web extensions").
enum ExtensionPackage {
  static let shimName = "compass-service-worker-shim.js"

  /// Unpacks `zip` into `directory` (made new) and adapts its manifest.
  static func unpack(_ zip: Data, to directory: URL) throws {
    let files = FileManager.default
    try? files.removeItem(at: directory)
    try files.createDirectory(at: directory, withIntermediateDirectories: true)
    do {
      try unzip(zip, to: directory)
      try adaptManifest(in: directory)
    } catch {
      try? files.removeItem(at: directory)
      throw error
    }
  }

  // MARK: ZIP

  /// Stored and deflated entries, from the central directory (sizes are
  /// there even when the local headers defer them). No ZIP64: extension
  /// packages are far below 4 GB.
  static func unzip(_ data: Data, to directory: URL) throws {
    let bytes = [UInt8](data)
    func u16(_ at: Int) -> Int { Int(bytes[at]) | Int(bytes[at + 1]) << 8 }
    func u32(_ at: Int) -> Int { u16(at) | u16(at + 2) << 16 }
    guard bytes.count >= 22 else { throw ExtensionError.badPackage }
    // The end of central directory record, within the last 64 KB + 22.
    var end = bytes.count - 22
    let floor = max(0, bytes.count - 22 - 65_535)
    while end >= floor, u32(end) != 0x0605_4B50 { end -= 1 }
    guard end >= floor else { throw ExtensionError.badPackage }
    let count = u16(end + 10)
    var at = u32(end + 16)
    let root = directory.standardizedFileURL.path
    for _ in 0..<count {
      guard at + 46 <= bytes.count, u32(at) == 0x0201_4B50 else { throw ExtensionError.badPackage }
      let method = u16(at + 10)
      let compressed = u32(at + 20)
      let size = u32(at + 24)
      let nameLength = u16(at + 28)
      let next = at + 46 + nameLength + u16(at + 30) + u16(at + 32)
      let local = u32(at + 42)
      guard at + 46 + nameLength <= bytes.count,
        let name = String(bytes: bytes[(at + 46)..<(at + 46 + nameLength)], encoding: .utf8)
      else { throw ExtensionError.badPackage }
      at = next
      let relative = name.drop { $0 == "/" }
      if relative.isEmpty || relative.hasSuffix("/") { continue }
      let target = directory.appendingPathComponent(String(relative)).standardizedFileURL
      // Nothing outside the extension's folder ("../").
      guard target.path.hasPrefix(root + "/") else { throw ExtensionError.badPackage }
      guard local + 30 <= bytes.count, u32(local) == 0x0403_4B50 else { throw ExtensionError.badPackage }
      let start = local + 30 + u16(local + 26) + u16(local + 28)
      guard start + compressed <= bytes.count else { throw ExtensionError.badPackage }
      let content: Data
      switch method {
      case 0:
        content = Data(bytes[start..<(start + compressed)])
      case 8:
        content = try inflate(bytes, start..<(start + compressed), size: size)
      default:
        throw ExtensionError.badPackage
      }
      try FileManager.default.createDirectory(
        at: target.deletingLastPathComponent(), withIntermediateDirectories: true)
      try content.write(to: target)
    }
  }

  /// Raw deflate (COMPRESSION_ZLIB is raw, without the zlib header).
  private static func inflate(_ bytes: [UInt8], _ range: Range<Int>, size: Int) throws -> Data {
    if size == 0 { return Data() }
    var output = [UInt8](repeating: 0, count: size)
    let written = bytes[range].withUnsafeBufferPointer { source in
      compression_decode_buffer(&output, size, source.baseAddress!, range.count, nil, COMPRESSION_ZLIB)
    }
    guard written == size else { throw ExtensionError.badPackage }
    return Data(output)
  }

  // MARK: Manifest

  static func adaptManifest(in directory: URL) throws {
    let url = directory.appendingPathComponent("manifest.json")
    guard var manifest = try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any] else {
      throw ExtensionError.badPackage
    }
    guard var background = manifest["background"] as? [String: Any],
      let worker = background["service_worker"] as? String
    else { return }
    let path = String(worker.drop { $0 == "/" })
    background["service_worker"] = nil
    if background["type"] as? String == "module" {
      background["scripts"] = [path]
    } else {
      let imports = importedScripts(of: path, in: directory)
      try "self.importScripts = function () {};\n".write(
        to: directory.appendingPathComponent(shimName), atomically: true, encoding: .utf8)
      background["scripts"] = [shimName] + imports + [path]
    }
    manifest["background"] = background
    let data = try JSONSerialization.data(withJSONObject: manifest, options: [.prettyPrinted, .sortedKeys])
    try data.write(to: url, options: .atomic)
  }

  /// The scripts `path` loads with importScripts("…", '…'), and theirs,
  /// first-loaded first, relative to the worker's folder (or the root, for
  /// "/…").
  static func importedScripts(of path: String, in directory: URL) -> [String] {
    var result: [String] = []
    var seen: Set<String> = [path]
    func visit(_ script: String) {
      guard let source = try? String(contentsOf: directory.appendingPathComponent(script), encoding: .utf8) else { return }
      let base = (script as NSString).deletingLastPathComponent
      for call in source.matches(of: /importScripts\s*\(([^)]*)\)/) {
        for literal in call.1.matches(of: /["']([^"']+)["']/) {
          let name = String(literal.1)
          let resolved = name.hasPrefix("/")
            ? String(name.dropFirst())
            : ((base as NSString).appendingPathComponent(name) as NSString).standardizingPath
          guard !seen.contains(resolved) else { continue }
          seen.insert(resolved)
          visit(resolved)
          result.append(resolved)
        }
      }
    }
    visit(path)
    return result
  }
}
