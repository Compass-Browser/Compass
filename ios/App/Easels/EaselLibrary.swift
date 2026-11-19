// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassEasels
import CompassModel
import SwiftUI
import UIKit
import WebKit

/// Easels on the phone (docs/design/easels.md; docs/design/ios.md,
/// "Easels"): CompassEasels' store in Application Support/Compass/Easels,
/// synced with the home account (SyncController), the editor in place of a
/// web view for compass://easel/ tabs (EaselScreen), and captures of page
/// regions (EaselCapture).
extension EaselStore {
  static let shared: EaselStore = {
    let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("Compass", isDirectory: true)
      .appendingPathComponent("Easels", isDirectory: true)
    return EaselStore(directory: directory)
  }()
}

/// A board's background (`CompassEasel.palette`).
enum EaselPalette: String, CaseIterable, Identifiable {
  case paper, grid, dots, dark
  var id: String { rawValue }

  init(_ name: String) { self = EaselPalette(rawValue: name) ?? .paper }

  var title: String { rawValue.capitalized }
  var isDark: Bool { self == .dark }
  var background: Color { isDark ? Color(red: 0.12, green: 0.12, blue: 0.13) : Color(red: 0.984, green: 0.98, blue: 0.969) }
  /// The default color of text and shapes.
  var ink: Color { isDark ? Color(red: 0.9, green: 0.93, blue: 0.95) : Color(red: 0.12, green: 0.14, blue: 0.16) }
  var lines: Color { isDark ? Color.white.opacity(0.08) : Color.black.opacity(0.07) }
}

/// The color palette for text and shapes, as CSS colors (`ink` is none:
/// the board's ink).
enum EaselColor: String, CaseIterable, Identifiable {
  case ink, red, orange, yellow, green, blue, purple, grey
  var id: String { rawValue }

  var css: String? {
    switch self {
    case .ink: nil
    case .red: "#d1242f"
    case .orange: "#e16f24"
    case .yellow: "#d4a72c"
    case .green: "#1a7f37"
    case .blue: "#0969da"
    case .purple: "#8250df"
    case .grey: "#6e7781"
    }
  }

  func color(on palette: EaselPalette) -> Color { css.flatMap(Self.parse) ?? palette.ink }

  /// "#rgb" or "#rrggbb"; anything else is the board's ink.
  static func parse(_ css: String) -> Color? {
    var hex = css.trimmingCharacters(in: .whitespaces)
    guard hex.hasPrefix("#") else { return nil }
    hex.removeFirst()
    if hex.count == 3 { hex = hex.map { "\($0)\($0)" }.joined() }
    guard hex.count == 6, let value = UInt32(hex, radix: 16) else { return nil }
    return Color(
      red: Double((value >> 16) & 0xff) / 255, green: Double((value >> 8) & 0xff) / 255,
      blue: Double(value & 0xff) / 255)
  }

  static func color(_ css: String, on palette: EaselPalette) -> Color {
    css.isEmpty ? palette.ink : parse(css) ?? palette.ink
  }
}

/// Decoded board images, by SHA-256.
@MainActor
enum EaselImageCache {
  private static let cache = NSCache<NSString, UIImage>()

  static func image(_ sha256: String) -> UIImage? {
    guard !sha256.isEmpty else { return nil }
    if let image = cache.object(forKey: sha256 as NSString) { return image }
    guard let data = EaselStore.shared.assetData(sha256), let image = UIImage(data: data) else { return nil }
    cache.setObject(image, forKey: sha256 as NSString)
    return image
  }
}

/// Where the editor last looked on each board (board points at the view's
/// center): a capture taken from a page lands there.
@MainActor
enum EaselViewMemory {
  static var centers: [String: CGPoint] = [:]

  /// The center for a new object of `size` on `board`: where the editor
  /// last looked, else below what's on the board.
  static func center(on board: String, size: CGSize) -> CGPoint {
    if let center = centers[board] { return center }
    let objects = EaselStore.shared.objects(of: board)
    guard let minX = objects.map(\.x).min(), let maxY = objects.map({ $0.y + $0.height }).max() else {
      return CGPoint(x: 40 + size.width / 2, y: 40 + size.height / 2)
    }
    return CGPoint(x: minX + size.width / 2, y: maxY + 40 + size.height / 2)
  }
}

extension AppModel {
  /// New Easel: a board, and a pinned tab for it in the shown space.
  func newEasel() {
    switch EaselStore.shared.createEasel(title: "Untitled Easel") {
    case .failure(let error): showMessage(error.message)
    case .success(let board):
      guard let tab = pinEasel(board) else { return }
      show(tab)
    }
  }

  /// A pinned tab for `board` in the shown space (not Incognito's).
  @discardableResult
  func pinEasel(_ board: EaselBoard) -> SidebarNode? {
    let space = selectedSpace.flatMap { $0.isIncognito ? nil : $0 } ?? store.spaces.first
    guard let space else { return nil }
    return store.addTab(url: EaselURL.url(board.uuid), title: board.title, to: space.pinned)
  }

  /// Opens `board` in `tab` (the Easels list's tab), else in a tab already
  /// on it, else in a new pinned tab.
  func openEasel(_ board: EaselBoard, in tab: SidebarNode? = nil) {
    let url = EaselURL.url(board.uuid)
    if let tab {
      store.update(tab, url: url, title: board.title)
      show(tab)
    } else if let existing = store.allNodes.first(where: { $0.isTab && EaselURL.uuid(from: $0.url) == board.uuid }) {
      show(existing)
    } else if let pinned = pinEasel(board) {
      show(pinned)
    }
  }

  /// A link or capture opened from a board: a new tab of the space.
  func openFromEasel(_ url: URL) {
    guard let space = selectedSpace ?? store.spaces.first,
      let tab = store.addTab(url: url.absoluteString, to: space.today)
    else { return }
    extensions.tabOpened(tab.id)
    show(tab)
  }

  #if DEBUG
    /// A board with one of each kind of object (-CompassEaselDemo).
    func easelDemo() {
      let easels = EaselStore.shared
      guard let board = try? easels.createEasel(title: "Demo Easel").get() else { return }
      func object(_ x: Double, _ y: Double, _ w: Double, _ h: Double, _ fill: (inout EaselObject) -> Void) -> EaselObject {
        var o = EaselObject()
        o.easelUuid = board.uuid
        o.x = x
        o.y = y
        o.width = w
        o.height = h
        fill(&o)
        return o
      }
      let objects = [
        object(20, 20, 300, 44) { $0.text.text = "Trip to Lisbon"; $0.text.style = "title" },
        object(20, 80, 280, 60) { $0.text.text = "Pastéis de nata at Belém, then the tram up to the castle."; $0.text.font = "hand" },
        object(20, 160, 140, 90) { $0.shape.kind = 1; $0.shape.color = "#0969da" },
        object(180, 160, 120, 90) { $0.shape.kind = 2; $0.shape.color = "#d1242f"; $0.shape.filled = true },
        object(20, 270, 160, 40) { $0.shape.kind = 3; $0.shape.points = [8, 32, 152, 8] },
        object(200, 270, 120, 60) { $0.shape.kind = 4; $0.shape.points = [8, 30, 30, 8, 60, 50, 90, 10, 112, 30]; $0.shape.color = "#1a7f37" },
        object(20, 350, 260, 90) { $0.link.url = "https://en.wikipedia.org/wiki/Lisbon"; $0.link.title = "Lisbon - Wikipedia"; $0.link.description_p = "Capital of Portugal" },
      ]
      _ = easels.save(objects)
      if let tab = pinEasel(board) { show(tab) }
    }
  #endif

  /// The tabs follow a board's title.
  func easelRenamed(_ board: String, title: String) {
    for node in store.allNodes where node.isTab && EaselURL.uuid(from: node.url) == board {
      store.update(node, title: title)
    }
  }
}

/// Link cards: the page's title, description and preview image.
enum EaselLinks {
  /// The page's preview (at most 1 MB of it read) and its image, fitted.
  static func fetch(_ url: URL, userAgent: String) async -> (EaselLinkPreview, EaselPreparedImage?) {
    var request = URLRequest(url: url, timeoutInterval: 15)
    request.setValue(userAgent, forHTTPHeaderField: "User-Agent")
    request.setValue("text/html,application/xhtml+xml", forHTTPHeaderField: "Accept")
    guard let (bytes, response) = try? await URLSession.shared.bytes(for: request) else { return (EaselLinkPreview(), nil) }
    var data = Data()
    do {
      for try await byte in bytes {
        data.append(byte)
        if data.count >= EaselLinkPreview.maxBytes { break }
      }
    } catch {}
    let html = String(decoding: data, as: UTF8.self)
    let preview = EaselLinkPreview.parse(html: html, baseURL: response.url ?? url)
    guard let imageURL = preview.imageURL else { return (preview, nil) }
    var imageRequest = URLRequest(url: imageURL, timeoutInterval: 15)
    imageRequest.setValue(userAgent, forHTTPHeaderField: "User-Agent")
    guard let (imageData, _) = try? await URLSession.shared.data(for: imageRequest), imageData.count <= 16 << 20 else {
      return (preview, nil)
    }
    let fitted = await Task.detached { try? EaselImages.fit(imageData).get() }.value
    return (preview, fitted)
  }
}

/// Captures of page regions and their Refresh (docs/design/easels.md,
/// "Captures").
@MainActor
enum EaselCapture {
  static let lastBoardKey = "EaselLastBoard"

  /// The boards to offer, the last one used first.
  static var boards: [EaselBoard] {
    let all = EaselStore.shared.easels
    guard let last = UserDefaults.standard.string(forKey: lastBoardKey), let i = all.firstIndex(where: { $0.uuid == last })
    else { return all }
    var list = all
    list.insert(list.remove(at: i), at: 0)
    return list
  }

  /// Where the page's viewport starts in the web view: below the status bar
  /// it runs under (iOS 26), or under a content inset.
  static func viewportTop(_ view: WKWebView) -> CGFloat {
    if #available(iOS 26.0, *) { return view.obscuredContentInsets.top }
    return view.scrollView.adjustedContentInset.top
  }

  /// The page's scroll and viewport size, in CSS pixels.
  static func pageGeometry(_ view: WKWebView) async -> (scroll: CGPoint, viewport: CGSize)? {
    let js = "[window.scrollX, window.scrollY, window.innerWidth, window.innerHeight]"
    guard let values = try? await view.evaluateJavaScript(js) as? [Double], values.count == 4, values[2] > 0 else {
      return nil
    }
    return (CGPoint(x: values[0], y: values[1]), CGSize(width: values[2], height: values[3]))
  }

  /// Takes `rect` (in the web view's points) of the page in `view` and
  /// adds it to `board`.
  static func capture(_ view: WKWebView, rect: CGRect, to board: String) async -> Result<Void, EaselError> {
    guard let page = view.url, ["http", "https"].contains(page.scheme?.lowercased() ?? "") else {
      return .failure(EaselError("Only web pages can be captured."))
    }
    let geometry = await pageGeometry(view)
    let viewport = geometry?.viewport ?? view.bounds.size
    let css = viewport.width / max(view.bounds.width, 1)
    let top = viewportTop(view)
    let config = WKSnapshotConfiguration()
    config.rect = rect
    guard let image = try? await view.takeSnapshot(configuration: config), let cg = image.cgImage else {
      return .failure(EaselError("The page couldn't be captured."))
    }
    guard let fitted = await Task.detached(operation: { EaselImages.fit(cg) }).value.success else {
      return .failure(EaselError("The capture couldn't be saved."))
    }
    let store = EaselStore.shared
    let sha: String
    switch store.addAsset(fitted.data, mimeType: fitted.mimeType) {
    case .success(let s): sha = s
    case .failure(let e): return .failure(e)
    }
    var object = EaselObject()
    object.easelUuid = board
    var c = SyncPb_CompassEaselCapture()
    c.url = page.absoluteString
    c.title = view.title ?? ""
    c.imageAsset = sha
    if let icon = await favicon(for: page.absoluteString) { c.faviconAsset = icon }
    c.regionX = rect.minX * css
    c.regionY = (rect.minY - top) * css
    c.regionWidth = rect.width * css
    c.regionHeight = rect.height * css
    c.scrollX = geometry.map { Double($0.scroll.x) } ?? 0
    c.scrollY = geometry.map { Double($0.scroll.y) } ?? 0
    c.viewportWidth = viewport.width
    c.viewportHeight = viewport.height
    c.captureTime = Easels.syncTime(Date())
    object.capture = c
    // At its CSS size.
    let size = CGSize(width: c.regionWidth, height: c.regionHeight)
    let center = EaselViewMemory.center(on: board, size: size)
    object.x = center.x - size.width / 2
    object.y = center.y - size.height / 2
    object.width = size.width
    object.height = size.height
    object.z = (store.objects(of: board).map(\.z).max() ?? 0) + 1
    switch store.save(object) {
    case .success:
      UserDefaults.standard.set(board, forKey: lastBoardKey)
      return .success(())
    case .failure(let e): return .failure(e)
    }
  }

  /// The site's icon as a board image, if Compass has one.
  static func favicon(for url: String) async -> String? {
    guard let icon = Favicons.shared.image(for: url), let png = icon.pngData() else { return nil }
    guard let fitted = await Task.detached(operation: { try? EaselImages.fit(png).get() }).value else { return nil }
    return try? EaselStore.shared.addAsset(fitted.data, mimeType: fitted.mimeType).get()
  }

  /// Holds the hidden page a Refresh loads: in the key window (WebKit
  /// doesn't draw or snapshot a view outside one), behind everything,
  /// nearly transparent.
  private static var host: UIView?

  /// Refresh: loads the capture's page in a hidden web view of the same
  /// viewport (the home profile's cookies), waits for the load and 1.5 s,
  /// scrolls where it was, and takes the region again.
  static func refresh(_ object: EaselObject, tabs: TabManager) async -> Result<Void, EaselError> {
    let c = object.capture
    guard let url = URL(string: c.url), let window = keyWindow else {
      return .failure(EaselError("The page couldn't be loaded."))
    }
    if host == nil || host?.window !== window {
      host?.removeFromSuperview()
      let view = UIView(frame: window.bounds)
      view.alpha = 0.01
      view.isUserInteractionEnabled = false
      window.insertSubview(view, at: 0)
      host = view
    }
    let view = tabs.makeWebView(profile: "", incognito: false, url: url)
    let size = CGSize(width: max(c.viewportWidth, 320), height: max(c.viewportHeight, 320))
    view.frame = CGRect(origin: .zero, size: size)
    host?.addSubview(view)
    defer { view.removeFromSuperview() }
    view.load(URLRequest(url: url))
    let deadline = Date().addingTimeInterval(30)
    try? await Task.sleep(for: .milliseconds(300))
    while view.isLoading, Date() < deadline { try? await Task.sleep(for: .milliseconds(200)) }
    try? await Task.sleep(for: .milliseconds(1500))
    _ = try? await view.evaluateJavaScript("window.scrollTo(\(c.scrollX), \(c.scrollY))")
    try? await Task.sleep(for: .milliseconds(300))
    let geometry = await pageGeometry(view)
    let css = (geometry?.viewport.width ?? size.width) / max(view.bounds.width, 1)
    // Where the page actually scrolled to (a shorter page stops early).
    let dy = (geometry.map { Double($0.scroll.y) } ?? c.scrollY) - c.scrollY
    let dx = (geometry.map { Double($0.scroll.x) } ?? c.scrollX) - c.scrollX
    let config = WKSnapshotConfiguration()
    config.rect = CGRect(
      x: (c.regionX - dx) / css, y: (c.regionY - dy) / css, width: c.regionWidth / css, height: c.regionHeight / css)
    guard let image = try? await view.takeSnapshot(configuration: config), let cg = image.cgImage else {
      return .failure(EaselError("The page couldn't be captured."))
    }
    guard let fitted = await Task.detached(operation: { EaselImages.fit(cg) }).value.success else {
      return .failure(EaselError("The capture couldn't be saved."))
    }
    let store = EaselStore.shared
    guard var current = store.object(object.uuid), current.hasCapture else {
      return .failure(EaselError("The capture was deleted."))
    }
    switch store.addAsset(fitted.data, mimeType: fitted.mimeType) {
    case .success(let sha):
      current.capture.imageAsset = sha
      current.capture.captureTime = Easels.syncTime(Date())
      if let title = view.title, !title.isEmpty { current.capture.title = title }
      return store.save(current).map { _ in () }
    case .failure(let e): return .failure(e)
    }
  }

  static var keyWindow: UIWindow? {
    UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.flatMap(\.windows).first { $0.isKeyWindow }
  }
}

extension Result {
  var success: Success? { if case .success(let value) = self { value } else { nil } }
}
