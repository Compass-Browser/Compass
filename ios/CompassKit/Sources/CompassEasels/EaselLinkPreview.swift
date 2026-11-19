// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// A link card's title, description and image from a page's HTML: Open
/// Graph (and Twitter) tags first, then <title> and the description meta.
public struct EaselLinkPreview: Equatable, Sendable {
  public var title: String = ""
  public var description: String = ""
  public var imageURL: URL?

  public init(title: String = "", description: String = "", imageURL: URL? = nil) {
    self.title = title
    self.description = description
    self.imageURL = imageURL
  }

  /// At most this much of a page is read.
  public static let maxBytes = 1024 * 1024

  public static func parse(html: String, baseURL: URL) -> EaselLinkPreview {
    var metas: [String: String] = [:]
    for tag in matches(#"<meta\b[^>]*>"#, in: html) {
      let attrs = attributes(tag)
      guard let key = (attrs["property"] ?? attrs["name"] ?? attrs["itemprop"])?.lowercased(),
        let content = attrs["content"], metas[key] == nil
      else { continue }
      metas[key] = decode(content)
    }
    var preview = EaselLinkPreview()
    let titleTag = firstGroup(#"<title\b[^>]*>([\s\S]*?)</title>"#, in: html).map(decode)
    preview.title = clean(metas["og:title"] ?? metas["twitter:title"] ?? titleTag ?? "", max: 300)
    preview.description = clean(
      metas["og:description"] ?? metas["twitter:description"] ?? metas["description"] ?? "", max: 500)
    if let image = metas["og:image:secure_url"] ?? metas["og:image"] ?? metas["og:image:url"]
      ?? metas["twitter:image"] ?? metas["twitter:image:src"],
      let url = URL(string: image.trimmingCharacters(in: .whitespaces), relativeTo: baseURL)?.absoluteURL,
      url.scheme == "https" || url.scheme == "http"
    {
      preview.imageURL = url
    }
    return preview
  }

  static func clean(_ text: String, max: Int) -> String {
    let collapsed = text.split(whereSeparator: \.isWhitespace).joined(separator: " ")
    return Easels.truncate(collapsed, max)
  }

  static func matches(_ pattern: String, in text: String) -> [String] {
    guard let re = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]) else { return [] }
    let ns = text as NSString
    return re.matches(in: text, range: NSRange(location: 0, length: ns.length)).map { ns.substring(with: $0.range) }
  }

  static func firstGroup(_ pattern: String, in text: String) -> String? {
    guard let re = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]),
      let m = re.firstMatch(in: text, range: NSRange(location: 0, length: (text as NSString).length)),
      m.numberOfRanges > 1
    else { return nil }
    return (text as NSString).substring(with: m.range(at: 1))
  }

  /// A tag's attributes, names lowercased: name="v", name='v' or name=v.
  static func attributes(_ tag: String) -> [String: String] {
    guard let re = try? NSRegularExpression(pattern: #"([a-zA-Z_:][-a-zA-Z0-9_:.]*)\s*=\s*("([^"]*)"|'([^']*)'|([^\s"'>]+))"#)
    else { return [:] }
    let ns = tag as NSString
    var out: [String: String] = [:]
    for m in re.matches(in: tag, range: NSRange(location: 0, length: ns.length)) {
      let name = ns.substring(with: m.range(at: 1)).lowercased()
      let value = (3...5).lazy.map { m.range(at: $0) }.first { $0.location != NSNotFound }.map(ns.substring(with:)) ?? ""
      if out[name] == nil { out[name] = value }
    }
    return out
  }

  /// HTML's common entities and numeric references.
  static func decode(_ text: String) -> String {
    guard text.contains("&") else { return text }
    var out = ""
    var rest = Substring(text)
    let named = ["amp": "&", "lt": "<", "gt": ">", "quot": "\"", "apos": "'", "#39": "'", "nbsp": "\u{a0}"]
    while let amp = rest.firstIndex(of: "&") {
      out += rest[..<amp]
      let after = rest[rest.index(after: amp)...]
      if let semi = after.prefix(10).firstIndex(of: ";") {
        let name = String(after[..<semi])
        var replacement = named[name.lowercased()]
        if replacement == nil, name.hasPrefix("#") {
          let digits = name.dropFirst()
          let value = digits.lowercased().hasPrefix("x") ? UInt32(digits.dropFirst(), radix: 16) : UInt32(digits)
          replacement = value.flatMap(Unicode.Scalar.init).map { String($0) }
        }
        if let replacement {
          out += replacement
          rest = after[after.index(after: semi)...]
          continue
        }
      }
      out += "&"
      rest = after
    }
    return out + rest
  }
}
