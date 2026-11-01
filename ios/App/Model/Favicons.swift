// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation
import Observation
import UIKit
import WebKit

/// Site icons by host, kept in memory and on disk (Caches/Favicons), so the
/// sidebar shows them at once and redraws don't refetch them. AsyncImage
/// had neither: a load cancelled by a redraw stayed a letter.
///
/// Where an icon comes from, best first: the icons a loaded page declares
/// (`pageLoaded`, or /favicon.ico when it declares none, as browsers do),
/// then an icon another device synced (`setSynced`), then a guess: the
/// host's apple-touch-icon.png, then /favicon.ico. Only page icons are
/// synced (`onPageIcon`; docs/design/sync.md, "Favicons").
@MainActor
@Observable
final class Favicons {
  static let shared = Favicons()

  private var images: [String: UIImage] = [:]
  @ObservationIgnored private var loading: Set<String> = []
  /// Hosts with no icon anywhere, not retried until the next launch.
  @ObservationIgnored private var missing: Set<String> = []
  /// Hosts whose icon came from their page, which beats the others.
  @ObservationIgnored private var fromPage: Set<String> = []
  /// Hosts whose icon came from sync, which beats a guess.
  @ObservationIgnored private var fromSync: Set<String> = []
  /// A page's icon, to sync: (host, 64 px PNG, source size, tab ID).
  @ObservationIgnored var onPageIcon: ((String, Data, Int, String) -> Void)?
  @ObservationIgnored private let directory: URL
  @ObservationIgnored private let session: URLSession

  /// Safari's, so sites don't refuse the app's default CFNetwork one.
  static let userAgent =
    "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1"

  private init() {
    directory = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Favicons", isDirectory: true)
    try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    let configuration = URLSessionConfiguration.default
    configuration.httpAdditionalHeaders = ["User-Agent": Self.userAgent]
    configuration.timeoutIntervalForRequest = 15
    session = URLSession(configuration: configuration)
  }

  static func host(of url: String) -> String? {
    guard let host = URL(string: url)?.host(), !host.isEmpty else { return nil }
    return host.lowercased()
  }

  /// The icon for a page's host, or nil while it loads (or if there's none).
  func image(for url: String) -> UIImage? {
    guard let host = Self.host(of: url) else { return nil }
    if let image = images[host] { return image }
    load(host)
    return nil
  }

  private func file(_ host: String) -> URL { directory.appendingPathComponent(host + ".png") }

  private func load(_ host: String) {
    guard !loading.contains(host), !missing.contains(host) else { return }
    loading.insert(host)
    Task {
      defer { loading.remove(host) }
      if let data = try? Data(contentsOf: file(host)), let image = UIImage(data: data) {
        images[host] = image
        return
      }
      let candidates = ["apple-touch-icon.png", "favicon.ico"].compactMap { URL(string: "https://\(host)/\($0)") }
      if let (image, _) = await fetch(candidates), !fromPage.contains(host), !fromSync.contains(host) {
        store(image, for: host)
      } else if images[host] == nil {
        missing.insert(host)
      }
    }
  }

  /// A loaded page's own icons (`<link rel=icon>`, apple-touch-icon), the
  /// largest first.
  func pageLoaded(_ view: WKWebView, tab: String) {
    guard let host = view.url?.host()?.lowercased(), view.url?.scheme?.hasPrefix("http") == true else { return }
    let script = """
      [...document.querySelectorAll('link[rel~="icon"], link[rel~="apple-touch-icon"], link[rel="apple-touch-icon-precomposed"]')]
        .filter(l => l.href && !/\\.svg(\\?|$)/i.test(l.href) && !/svg/.test(l.type))
        .map(l => ({href: l.href, size: Math.max(0, ...(l.sizes && l.sizes.value || '').split(/\\s+/).map(s => parseInt(s) || 0)),
                    touch: /apple-touch-icon/.test(l.rel)}))
        .sort((a, b) => (b.size || (b.touch ? 180 : 16)) - (a.size || (a.touch ? 180 : 16)))
        .map(l => l.href)
      """
    view.evaluateJavaScript(script) { [weak self] result, _ in
      var urls = (result as? [String] ?? []).compactMap(URL.init(string:))
      // None declared: what browsers show, the host's /favicon.ico.
      if urls.isEmpty, let ico = URL(string: "https://\(host)/favicon.ico") { urls = [ico] }
      Task { @MainActor in
        guard let self, let (image, sourceSize) = await self.fetch(urls) else { return }
        self.fromPage.insert(host)
        self.missing.remove(host)
        self.store(image, for: host)
        if let png = Self.syncPNG(image) { self.onPageIcon?(host, png, sourceSize, tab) }
      }
    }
  }

  /// An icon another device got from its page: better than a guess, not
  /// than this phone's own page icon.
  func setSynced(host: String, png: Data) {
    let host = host.lowercased()
    guard !fromPage.contains(host), let image = UIImage(data: png) else { return }
    fromSync.insert(host)
    missing.remove(host)
    store(image, for: host)
  }

  /// At most 64 px a side and 8 KiB, as sync takes it.
  static func syncPNG(_ image: UIImage) -> Data? {
    for side in [64.0, 48, 32] {
      if let data = downscaled(image, to: side).pngData(), data.count <= 8 * 1024 { return data }
    }
    return nil
  }

  private func store(_ image: UIImage, for host: String) {
    images[host] = image
    if let data = image.pngData() { try? data.write(to: file(host), options: .atomic) }
  }

  /// The first candidate that is an image, scaled down to at most 128 px,
  /// with its larger side in px before that.
  private func fetch(_ urls: [URL]) async -> (UIImage, Int)? {
    for url in urls {
      guard let (data, response) = try? await session.data(from: url),
        (response as? HTTPURLResponse)?.statusCode == 200,
        let image = UIImage(data: data), image.size.width >= 8
      else { continue }
      let side = Int(max(image.size.width, image.size.height) * image.scale)
      return (Self.downscaled(image, to: 128), side)
    }
    return nil
  }

  private static func downscaled(_ image: UIImage, to limit: CGFloat) -> UIImage {
    let side = max(image.size.width, image.size.height) * image.scale
    guard side > limit else { return image }
    let scale = limit / side
    let size = CGSize(width: image.size.width * image.scale * scale, height: image.size.height * image.scale * scale)
    let format = UIGraphicsImageRendererFormat()
    format.scale = 1
    return UIGraphicsImageRenderer(size: size, format: format).image { _ in
      image.draw(in: CGRect(origin: .zero, size: size))
    }
  }
}
