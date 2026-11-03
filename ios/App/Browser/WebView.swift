// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI
import WebKit

/// Hosts an existing WKWebView. Web views are owned by the live-tab registry,
/// not by SwiftUI, so switching entries never reloads a page.
///
/// The web view sits in a container of ours, never as SwiftUI's own view:
/// with several windows (iPad) a page can move to another window, and
/// SwiftUI taking this view down afterwards must not pull it out of there.
/// The container takes the page back whenever it updates.
///
/// `bottomInset` is the floating chrome over the page's bottom edge: the page
/// scrolls under it, and its end still scrolls clear of it.
///
/// `topInset` is the status bar over the page's top edge. The page scrolls
/// under it too, but WebKit lays out fixed and sticky elements (YouTube's
/// header) below it, as in Safari (`obscuredContentInsets`, iOS 26).
/// Before iOS 26 WebKit has no public way to do that, so the page starts
/// below the status bar instead, and the strip behind the status bar takes
/// the page's theme color (or its background), as Safari does on iOS 18.
///
/// `obscuredBottom` is the same at the bottom: the bar and the home
/// indicator, so fixed footers (YouTube's tab bar) sit above the bar.
struct WebView: UIViewRepresentable {
  let webView: WKWebView
  var bottomInset: CGFloat = 0
  var topInset: CGFloat = 0
  var obscuredBottom: CGFloat = 0

  func makeUIView(context: Context) -> Container {
    let container = Container()
    container.backgroundColor = .clear
    adopt(into: container)
    return container
  }

  func updateUIView(_ container: Container, context: Context) { adopt(into: container) }

  private func adopt(into container: Container) {
    if webView.superview !== container {
      webView.removeFromSuperview()
      container.addSubview(webView)
      container.watch(webView)
    }
    if #unavailable(iOS 26.0) { container.statusHeight = topInset }
    container.setNeedsLayout()
    apply(to: webView)
  }

  /// Holds the page. Before iOS 26 it lays the page out below the status
  /// bar and fills the status bar's strip with the page's color.
  final class Container: UIView {
    var statusHeight: CGFloat = 0 {
      didSet { if statusHeight != oldValue { setNeedsLayout(); updateColor() } }
    }
    private weak var page: WKWebView?
    private var observations: [NSKeyValueObservation] = []

    func watch(_ view: WKWebView) {
      page = view
      observations = []
      if #unavailable(iOS 26.0) {
        observations = [
          view.observe(\.themeColor) { [weak self] _, _ in self?.updateColor() },
          view.observe(\.underPageBackgroundColor) { [weak self] _, _ in self?.updateColor() },
        ]
      }
      updateColor()
    }

    private func updateColor() {
      guard let page, statusHeight > 0 else { backgroundColor = .clear; return }
      backgroundColor = page.themeColor ?? page.underPageBackgroundColor
    }

    override func layoutSubviews() {
      super.layoutSubviews()
      guard let page, page.superview === self else { return }
      let top = min(statusHeight, bounds.height)
      page.frame = CGRect(x: 0, y: top, width: bounds.width, height: bounds.height - top)
    }
  }

  private func apply(to view: WKWebView) {
    if #available(iOS 26.0, *) {
      let insets = UIEdgeInsets(top: topInset, left: 0, bottom: obscuredBottom, right: 0)
      if view.obscuredContentInsets != insets { view.obscuredContentInsets = insets }
    }
    let scroll = view.scrollView
    guard scroll.contentInset.bottom != bottomInset else { return }
    scroll.contentInset.bottom = bottomInset
    scroll.verticalScrollIndicatorInsets.bottom = bottomInset
  }
}
