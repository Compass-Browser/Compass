// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Where the command bar searches (Settings → Search Engine). DuckDuckGo
/// by default, as before there was a choice.
enum SearchEngine: String, CaseIterable, Identifiable {
  case duckDuckGo, google, bing, ecosia, brave, startpage

  static let key = "searchEngine"

  /// The chosen one.
  static var current: SearchEngine {
    UserDefaults.standard.string(forKey: key).flatMap(SearchEngine.init(rawValue:)) ?? .duckDuckGo
  }

  var id: String { rawValue }

  var name: String {
    switch self {
    case .duckDuckGo: "DuckDuckGo"
    case .google: "Google"
    case .bing: "Bing"
    case .ecosia: "Ecosia"
    case .brave: "Brave Search"
    case .startpage: "Startpage"
    }
  }

  /// The results page for `query`.
  func url(for query: String) -> String {
    let (base, name) =
      switch self {
      case .duckDuckGo: ("https://duckduckgo.com/", "q")
      case .google: ("https://www.google.com/search", "q")
      case .bing: ("https://www.bing.com/search", "q")
      case .ecosia: ("https://www.ecosia.org/search", "q")
      case .brave: ("https://search.brave.com/search", "q")
      case .startpage: ("https://www.startpage.com/do/search", "query")
      }
    var c = URLComponents(string: base)!
    c.queryItems = [URLQueryItem(name: name, value: query)]
    return c.url!.absoluteString
  }
}
