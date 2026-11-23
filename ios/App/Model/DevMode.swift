// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import Foundation
import Observation
import WebKit

/// Development mode, iOS's lite version (docs/design/dev-mode.md, "iOS"):
/// on its sites, reload skips the cache and a bar above the page shows the
/// full URL with a Copy button. WKWebView has no DevTools of its own (Safari's
/// Web Inspector on a Mac attaches instead). The sites stay on the device
/// (UserDefaults "DevModeHosts"), as the desktop's do.
@MainActor @Observable
final class DevMode {
  static let shared = DevMode()

  private static let key = "DevModeHosts"
  private(set) var sites: DevModeSites

  private init() {
    // Never saved: local development servers, as on the desktop.
    sites = DevModeSites(UserDefaults.standard.stringArray(forKey: Self.key) ?? ["localhost"])
  }

  var patterns: [String] { sites.patterns }

  /// A page at `url` is in development mode.
  func isOn(_ url: URL?) -> Bool { sites.contains(url) }

  /// A web page: development mode can be on for it.
  func canToggle(_ url: URL?) -> Bool {
    ["http", "https"].contains(url?.scheme?.lowercased() ?? "")
  }

  func toggle(_ url: URL?) {
    guard let url, canToggle(url) else { return }
    sites.set(url, enabled: !isOn(url))
    save()
  }

  /// The pattern kept, or nil when `input` isn't a site.
  @discardableResult
  func add(_ input: String) -> String? {
    let pattern = sites.add(input)
    save()
    return pattern
  }

  func remove(_ pattern: String) {
    sites.remove(pattern)
    save()
  }

  /// Reloads `view`, from the network on a site in development mode.
  func reload(_ view: WKWebView?) {
    guard let view else { return }
    if isOn(view.url) { view.reloadFromOrigin() } else { view.reload() }
  }

  private func save() {
    UserDefaults.standard.set(sites.patterns, forKey: Self.key)
  }
}
