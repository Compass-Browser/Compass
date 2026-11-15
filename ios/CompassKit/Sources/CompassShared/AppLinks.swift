// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Links that leave the browser for an app (docs/design/ios.md, "Links to
/// apps"): what a tab does with a navigation, before WebKit loads it.
public enum AppLinks {
  /// What to do with one navigation.
  public enum Decision: Equatable, Sendable {
    /// The browser loads it (or WebKit decides, as before).
    case allow
    /// Cancelled, nothing opens: a frame going to an app, or Compass's
    /// own links, which pages don't get to use.
    case ignore
    /// Cancelled and handed to iOS, which asks first itself (tel:,
    /// mailto:, sms:, facetime:).
    case openSystem(URL)
    /// Cancelled and handed to iOS at once: a tapped link to an app.
    case openApp(URL)
    /// Cancelled; "Open in app?" first: a page going to an app without a
    /// tap (a redirect, a script).
    case askToOpenApp(URL)
    /// Cancelled; iOS opens the app that claims the link (a universal
    /// link), else the tab loads it after all.
    case tryUniversalLink(URL)
  }

  /// What WebKit says about the navigation, and what the tab knows.
  public struct Navigation: Sendable {
    public var url: URL
    /// The page the tab shows (nil: none yet).
    public var page: URL?
    public var isMainFrame: Bool
    /// A tapped link (`WKNavigationType.linkActivated`).
    public var isUserTap: Bool
    public var isIncognito: Bool
    /// "Open Links in Apps" for the page's site and the link's (both must
    /// be on).
    public var openLinksInApps: Bool
    /// The tab is loading this URL after iOS had no app for it.
    public var isRetry: Bool

    public init(
      url: URL, page: URL?, isMainFrame: Bool, isUserTap: Bool, isIncognito: Bool = false,
      openLinksInApps: Bool = true, isRetry: Bool = false
    ) {
      self.url = url
      self.page = page
      self.isMainFrame = isMainFrame
      self.isUserTap = isUserTap
      self.isIncognito = isIncognito
      self.openLinksInApps = openLinksInApps
      self.isRetry = isRetry
    }
  }

  /// Schemes the web view loads itself.
  public static let browserSchemes: Set<String> = [
    "http", "https", "about", "data", "blob", "javascript", "file", "webkit-extension", ReaderPageFiles.scheme,
  ]
  /// Compass's own (DeepLink, pairing): never from a page.
  public static let ownSchemes: Set<String> = [DeepLink.scheme, "compass-pair"]
  /// iOS asks before these itself.
  public static let systemSchemes: Set<String> = ["tel", "mailto", "sms", "facetime", "facetime-audio"]

  public static func decide(_ n: Navigation) -> Decision {
    let scheme = n.url.scheme?.lowercased() ?? ""
    if scheme.isEmpty || browserSchemes.contains(scheme) {
      return universalLink(n) ? .tryUniversalLink(n.url) : .allow
    }
    // A frame doesn't take the page into an app, and fails quietly.
    guard n.isMainFrame, !ownSchemes.contains(scheme) else { return .ignore }
    if systemSchemes.contains(scheme) { return .openSystem(n.url) }
    return n.isUserTap ? .openApp(n.url) : .askToOpenApp(n.url)
  }

  /// A tapped https link to another site, outside incognito, with the
  /// setting on.
  private static func universalLink(_ n: Navigation) -> Bool {
    guard n.isMainFrame, n.isUserTap, !n.isIncognito, n.openLinksInApps, !n.isRetry,
      n.url.scheme?.lowercased() == "https", let page = n.page,
      ["http", "https"].contains(page.scheme?.lowercased() ?? "")
    else { return false }
    return !sameSite(n.url, page)
  }

  /// Same host, or one a subdomain of the other ("www." aside). No public
  /// suffix list, so sibling subdomains (a.example.com, b.example.com)
  /// count as different sites.
  public static func sameSite(_ a: URL, _ b: URL) -> Bool {
    guard let x = site(a), let y = site(b) else { return false }
    return x == y || x.hasSuffix("." + y) || y.hasSuffix("." + x)
  }

  private static func site(_ url: URL) -> String? {
    guard var host = url.host()?.lowercased(), !host.isEmpty else { return nil }
    if host.hasSuffix(".") { host.removeLast() }
    return host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
  }
}
