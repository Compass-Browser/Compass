// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Observation

/// Per-site switches (Site Settings, from the page menu), with defaults for
/// every site (Global Site Settings). A site keeps only what it changed, so
/// a later change to the defaults still reaches the rest. Phone-local.
@MainActor @Observable
final class SiteSettings {
  enum Option: String, Codable, CaseIterable {
    case blockAds, blockCookieBanners, collapseBar, swipeToNavigate, desktopSite

    /// Off for desktopSite, on for the rest.
    var defaultValue: Bool { self != .desktopSite }
  }

  private(set) var global: [Option: Bool] = [:]
  private(set) var sites: [String: [Option: Bool]] = [:]
  /// After a change: the option and the site (nil: the default for every
  /// site), so open tabs follow at once (AppModel).
  @ObservationIgnored var onChange: ((Option, String?) -> Void)?
  private let file: URL

  init(directory: URL) {
    file = directory.appendingPathComponent("SiteSettings.json")
    if let data = try? Data(contentsOf: file), let saved = try? JSONDecoder().decode(Saved.self, from: data) {
      global = saved.global
      sites = saved.sites
    }
  }

  /// The site's key: its host without "www.".
  static func site(_ url: URL?) -> String? {
    guard let host = url?.host()?.lowercased(), !host.isEmpty else { return nil }
    return host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
  }

  /// The default for every site.
  func value(_ option: Option) -> Bool { global[option] ?? option.defaultValue }

  /// For `site` (nil: the default).
  func value(_ option: Option, site: String?) -> Bool {
    site.flatMap { sites[$0]?[option] } ?? value(option)
  }

  /// The site changed it from the default.
  func isOverridden(_ option: Option, site: String) -> Bool { sites[site]?[option] != nil }

  /// Sets it for `site`, or the default when nil. A site set back to the
  /// default drops its own value.
  func set(_ option: Option, _ on: Bool, site: String?) {
    if let site {
      var own = sites[site] ?? [:]
      own[option] = on == value(option) ? nil : on
      sites[site] = own.isEmpty ? nil : own
    } else {
      global[option] = on == option.defaultValue ? nil : on
    }
    save()
    onChange?(option, site)
  }

  private struct Saved: Codable {
    var global: [Option: Bool]
    var sites: [String: [Option: Bool]]
  }

  private func save() {
    if let data = try? JSONEncoder().encode(Saved(global: global, sites: sites)) {
      try? data.write(to: file, options: .atomic)
    }
  }
}
