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
/// (`pageLoaded`), then the host's apple-touch-icon.png, then /favicon.ico.
@MainActor
@Observable
final class Favicons {
  static let shared = Favicons()

  private var images: [String: UIImage] = [:]
  @ObservationIgnored private var loading: Set<String> = []
  /// Hosts with no icon anywhere, not retried until the next launch.
  @ObservationIgnored private var missing: Set<String> = []
  /// Hosts whose icon came from their page, which beats the guessed paths.
  @ObservationIgnored private var fromPage: Set<String> = []
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
      if let image = await fetch(candidates), !fromPage.contains(host) {
        store(image, for: host)
      } else if images[host] == nil {
        missing.insert(host)
      }
    }
  }

  /// A loaded page's own icons (`<link rel=icon>`, apple-touch-icon), the
  /// largest first.
  func pageLoaded(_ view: WKWebView) {
    guard let host = view.url?.host()?.lowercased() else { return }
    let script = """
      [...document.querySelectorAll('link[rel~="icon"], link[rel~="apple-touch-icon"], link[rel="apple-touch-icon-precomposed"]')]
        .filter(l => l.href && !/\\.svg(\\?|$)/i.test(l.href) && !/svg/.test(l.type))
        .map(l => ({href: l.href, size: Math.max(0, ...(l.sizes && l.sizes.value || '').split(/\\s+/).map(s => parseInt(s) || 0)),
                    touch: /apple-touch-icon/.test(l.rel)}))
        .sort((a, b) => (b.size || (b.touch ? 180 : 16)) - (a.size || (a.touch ? 180 : 16)))
        .map(l => l.href)
      """
    view.evaluateJavaScript(script) { [weak self] result, _ in
      let urls = (result as? [String] ?? []).compactMap(URL.init(string:))
      guard !urls.isEmpty else { return }
      Task { @MainActor in
        guard let self, let image = await self.fetch(urls) else { return }
        self.fromPage.insert(host)
        self.missing.remove(host)
        self.store(image, for: host)
      }
    }
  }

  private func store(_ image: UIImage, for host: String) {
    images[host] = image
    if let data = image.pngData() { try? data.write(to: file(host), options: .atomic) }
  }

  /// The first candidate that is an image, scaled down to at most 128 px.
  private func fetch(_ urls: [URL]) async -> UIImage? {
    for url in urls {
      guard let (data, response) = try? await session.data(from: url),
        (response as? HTTPURLResponse)?.statusCode == 200,
        let image = UIImage(data: data), image.size.width >= 8
      else { continue }
      return Self.downscaled(image)
    }
    return nil
  }

  private static func downscaled(_ image: UIImage) -> UIImage {
    let side = max(image.size.width, image.size.height) * image.scale
    guard side > 128 else { return image }
    let scale = 128 / side
    let size = CGSize(width: image.size.width * image.scale * scale, height: image.size.height * image.scale * scale)
    let format = UIGraphicsImageRendererFormat()
    format.scale = 1
    return UIGraphicsImageRenderer(size: size, format: format).image { _ in
      image.draw(in: CGRect(origin: .zero, size: size))
    }
  }
}
