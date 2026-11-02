// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// The app's own links (`compass:`), which widgets, the icon menu,
/// Spotlight and the extensions open.
public enum DeepLink: Equatable, Sendable {
  /// A web page in a new Today tab, in the space (nil = the shown one).
  case open(url: String, space: String?)
  /// A sidebar entry (favorite, Pinned or Today).
  case node(String)
  /// A space's sidebar.
  case space(String)
  /// The command bar, for a search or an address.
  case search
  /// The command bar for a new tab, in the space (nil = the shown one).
  case newTab(space: String?)
  /// Take the Inbox's requests.
  case inbox
  /// The downloads list.
  case downloads

  public static let scheme = "compass"

  public init?(url: URL) {
    guard url.scheme?.lowercased() == Self.scheme else { return nil }
    let parts = URLComponents(url: url, resolvingAgainstBaseURL: false)
    func query(_ name: String) -> String? {
      parts?.queryItems?.first { $0.name == name }?.value.flatMap { $0.isEmpty ? nil : $0 }
    }
    // compass://node/<id>: host "node", path "/<id>".
    let arg = url.path().split(separator: "/").first.map { String($0).removingPercentEncoding ?? String($0) }
    switch url.host()?.lowercased() {
    case "open":
      guard let target = query("url") else { return nil }
      self = .open(url: target, space: query("space"))
    case "node":
      guard let arg else { return nil }
      self = .node(arg)
    case "space":
      guard let arg else { return nil }
      self = .space(arg)
    case "search": self = .search
    case "new": self = .newTab(space: query("space"))
    case "inbox": self = .inbox
    case "downloads": self = .downloads
    default: return nil
    }
  }

  public var url: URL {
    var parts = URLComponents()
    parts.scheme = Self.scheme
    var items: [URLQueryItem] = []
    switch self {
    case .open(let target, let space):
      parts.host = "open"
      items.append(URLQueryItem(name: "url", value: target))
      if let space { items.append(URLQueryItem(name: "space", value: space)) }
    case .node(let id):
      parts.host = "node"
      parts.path = "/" + id
    case .space(let id):
      parts.host = "space"
      parts.path = "/" + id
    case .search: parts.host = "search"
    case .newTab(let space):
      parts.host = "new"
      if let space { items.append(URLQueryItem(name: "space", value: space)) }
    case .inbox: parts.host = "inbox"
    case .downloads: parts.host = "downloads"
    }
    if !items.isEmpty {
      parts.queryItems = items
      // URLComponents leaves "&", "=" and "+" in values; a URL inside a
      // query needs them escaped.
      var allowed = CharacterSet.urlQueryAllowed
      allowed.remove(charactersIn: "&=+?#")
      parts.percentEncodedQuery = items.map {
        "\($0.name)=\(($0.value ?? "").addingPercentEncoding(withAllowedCharacters: allowed) ?? "")"
      }.joined(separator: "&")
    }
    return parts.url!
  }
}
