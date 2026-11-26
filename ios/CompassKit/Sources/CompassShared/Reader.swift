// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

// Reader mode's pure parts (docs/design/reader-mode.md, "iOS"). The shared
// package (common/reader/) does the work in web views; these are what the
// app decides around it: the reader settings (kept in UserDefaults and
// handed to reader_view.js through `compassReaderHost`), the reader page's
// files and Content Security Policy, reader.js's answers, and what a link
// in the article does.

/// The reader's look: reader_view.js's `{font, size, width, theme}`, with
/// its choices and defaults. Kept in UserDefaults as the JSON the desktop
/// keeps in its reader origin's localStorage, under the same key.
public struct ReaderSettings: Equatable, Sendable {
  public static let fonts = ["serif", "sans"]
  public static let sizes = [14, 15, 17, 19, 21, 24, 28, 32]
  public static let widths = ["narrow", "medium", "wide"]
  /// "auto" follows the system's light or dark.
  public static let themes = ["auto", "light", "sepia", "dark"]
  public static let key = "compass.reader.settings"

  public var font = "serif"
  public var size = 19
  public var width = "medium"
  public var theme = "auto"

  public init() {}

  /// reader_view.js's `validSettings()`: each known value is taken, the
  /// rest are the defaults.
  public init(_ value: Any?) {
    guard let object = value as? [String: Any] else { return }
    if let font = object["font"] as? String, Self.fonts.contains(font) { self.font = font }
    if let width = object["width"] as? String, Self.widths.contains(width) { self.width = width }
    if let theme = object["theme"] as? String, Self.themes.contains(theme) { self.theme = theme }
    if let size = object["size"] as? NSNumber, !(object["size"] is Bool),
      Double(size.intValue) == size.doubleValue, Self.sizes.contains(size.intValue)
    {
      self.size = size.intValue
    }
  }

  public var object: [String: Any] { ["font": font, "size": size, "width": width, "theme": theme] }

  /// `{"font":…,"size":…,"theme":…,"width":…}`, keys sorted.
  public var json: String {
    let data = (try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])) ?? Data("{}".utf8)
    return String(decoding: data, as: UTF8.self)
  }

  public init(defaults: UserDefaults) {
    let text = defaults.string(forKey: Self.key)
    self.init(text.flatMap { try? JSONSerialization.jsonObject(with: Data($0.utf8)) })
  }

  public func save(to defaults: UserDefaults) {
    defaults.set(json, forKey: Self.key)
  }
}

/// What reader.js's `check()` and `parse()` said.
public struct ReaderResult: Equatable, Sendable {
  public var ok: Bool
  public var readerable: Bool
  public var title: String
  public var error: String
  /// The answer as JSON, for `compassReaderView.render()`.
  public var json: String

  /// From the JSON text the app gets back (`JSON.stringify` of the result);
  /// anything unreadable is a failed answer.
  public init(json text: String?) {
    let object = text.flatMap { try? JSONSerialization.jsonObject(with: Data($0.utf8)) } as? [String: Any]
    ok = object?["ok"] as? Bool == true
    readerable = object?["readerable"] as? Bool == true
    title = object?["title"] as? String ?? ""
    error = object?["error"] as? String ?? ""
    json = object != nil ? text ?? "" : Self.failure(String(localized: "This page has no article.", bundle: .module, comment: "Reader: the page has no article to show."))
  }

  /// A failed parse(), as the view shows it ("This page has no article").
  public static func failure(_ message: String) -> String {
    let data = (try? JSONSerialization.data(withJSONObject: ["ok": false, "error": message], options: [.sortedKeys]))
    return data.map { String(decoding: $0, as: UTF8.self) } ?? #"{"ok":false}"#
  }
}

/// The reader page: the shared files, served by the app under its own
/// scheme, with the desktop's Content Security Policy.
public enum ReaderPageFiles {
  public static let scheme = "compass-reader"
  public static let host = "reader"
  /// The page to load.
  public static var pageURL: URL { URL(string: "\(scheme)://\(host)/reader.html")! }

  /// The files the page may load, by path: (bundle resource name,
  /// extension, MIME type). Nothing else is served.
  public static let files: [String: (name: String, ext: String, type: String)] = [
    "/reader.html": ("reader", "html", "text/html"),
    "/reader.css": ("reader", "css", "text/css"),
    "/reader.js": ("reader", "js", "text/javascript"),
    "/reader_view.js": ("reader_view", "js", "text/javascript"),
  ]

  /// The desktop's policy (docs/design/reader-mode.md, "The reader page"):
  /// only the page's own scripts run, whatever got into the article.
  public static let contentSecurityPolicy =
    "default-src 'none'; script-src 'self'; style-src 'self'; img-src https: http: data:; "
    + "font-src 'self'; media-src 'none'; connect-src 'none'; frame-src 'none'; "
    + "form-action 'none'; base-uri 'none'"

  /// reader.html with the policy also as a `<meta>`, first in its head.
  public static func page(_ html: String) -> String {
    let meta = #"<meta http-equiv="Content-Security-Policy" content="\#(contentSecurityPolicy)">"#
    guard let head = html.range(of: "<head>") else { return meta + html }
    return html.replacingCharacters(in: head, with: "<head>\n" + meta)
  }

  /// The script that runs first in the reader page (the page's world):
  /// reader_view.js's `compassReaderHost`, with the settings the app keeps
  /// and the close button going to the app. Esc with the settings panel
  /// closed leaves too (the view stops it while the panel is open).
  public static func hostScript(settings: ReaderSettings, handler: String) -> String {
    """
    (() => {
      const post = (message) => window.webkit.messageHandlers.\(handler).postMessage(message);
      const settings = \(settings.json);
      globalThis.compassReaderHost = {
        loadSettings: () => ({...settings}),
        saveSettings: (value) => post({settings: value}),
        close: () => post({close: true}),
      };
      window.addEventListener('keydown', (e) => {
        if (e.key === 'Escape' && !e.defaultPrevented) post({close: true});
      });
    })();
    """
  }
}

/// What a tap on a link in the reader does.
public enum ReaderLink: Equatable, Sendable {
  /// The reader page itself (its first load).
  case stay
  /// The article's own page ("Original page"): back to it, Reader closes.
  case leave
  /// A web page, or a spot in the article's page: the tab loads it.
  case openInTab(URL)
  /// mailto: and the like: the system opens it.
  case system(URL)
  /// Anything else: nothing.
  case ignore

  /// `url` is where the link goes, `page` the article's page.
  public static func decide(_ url: URL, page: URL) -> ReaderLink {
    let scheme = url.scheme?.lowercased() ?? ""
    if scheme == ReaderPageFiles.scheme || url.absoluteString == "about:blank" { return .stay }
    if ["http", "https"].contains(scheme) {
      return url == page || (url.fragment == nil && withoutFragment(url) == withoutFragment(page))
        ? .leave : .openInTab(url)
    }
    if ["mailto", "tel", "sms"].contains(scheme) { return .system(url) }
    return .ignore
  }

  private static func withoutFragment(_ url: URL) -> String {
    var components = URLComponents(url: url, resolvingAgainstBaseURL: false)
    components?.fragment = nil
    return components?.string ?? url.absoluteString
  }
}
