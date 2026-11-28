// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// The user's translation choices, in UserDefaults (not synced, as the
/// desktop's translate prefs on another device would be the desktop's to
/// own): languages to translate without asking, and languages and sites
/// never to offer.
public struct TranslationPrefs {
  public static let alwaysKey = "TranslateAlways"
  public static let neverKey = "TranslateNever"
  public static let neverSitesKey = "TranslateNeverSites"
  /// Whether pages are offered at all (Settings).
  public static let offerKey = "TranslateOffer"

  private let defaults: UserDefaults

  public init(defaults: UserDefaults = .standard) {
    self.defaults = defaults
  }

  public var offers: Bool {
    get { defaults.object(forKey: Self.offerKey) as? Bool ?? true }
    nonmutating set { defaults.set(newValue, forKey: Self.offerKey) }
  }

  /// Source language → the language to translate it into, without asking.
  public var always: [String: String] {
    get { defaults.dictionary(forKey: Self.alwaysKey) as? [String: String] ?? [:] }
    nonmutating set { defaults.set(newValue, forKey: Self.alwaysKey) }
  }

  public var never: Set<String> {
    get { Set(defaults.stringArray(forKey: Self.neverKey) ?? []) }
    nonmutating set { defaults.set(newValue.sorted(), forKey: Self.neverKey) }
  }

  public var neverSites: Set<String> {
    get { Set(defaults.stringArray(forKey: Self.neverSitesKey) ?? []) }
    nonmutating set { defaults.set(newValue.sorted(), forKey: Self.neverSitesKey) }
  }

  /// A site's key: its host without `www.`.
  public static func site(_ url: URL?) -> String? {
    guard let host = url?.host()?.lowercased(), !host.isEmpty else { return nil }
    return host.hasPrefix("www.") ? String(host.dropFirst(4)) : host
  }

  /// Always translating `source` (into `target`), or not. Never and always
  /// exclude each other, as on the desktop.
  public func setAlways(_ source: String, _ target: String?) {
    var always = self.always
    always[source] = target
    self.always = always
    if target != nil { never.remove(source) }
  }

  public func setNever(_ source: String) {
    never.insert(source)
    setAlways(source, nil)
  }

  public func setNeverSite(_ url: URL?) {
    if let site = Self.site(url) { neverSites.insert(site) }
  }

  /// Whether to offer translating `source` on `url`.
  public func shouldOffer(source: String?, url: URL?, preferred: [String]) -> Bool {
    guard offers, !(Self.site(url).map(neverSites.contains) ?? false) else { return false }
    return PageLanguage.shouldOffer(source: source, preferred: preferred, never: never)
  }
}
