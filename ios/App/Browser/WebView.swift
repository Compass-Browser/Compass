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
///
/// `obscuredBottom` is the same at the bottom: the bar and the home
/// indicator, so fixed footers (YouTube's tab bar) sit above the bar.
struct WebView: UIViewRepresentable {
  let webView: WKWebView
  var bottomInset: CGFloat = 0
  var topInset: CGFloat = 0
  var obscuredBottom: CGFloat = 0

  func makeUIView(context: Context) -> UIView {
    let container = UIView()
    container.backgroundColor = .clear
    adopt(into: container)
    return container
  }

  func updateUIView(_ container: UIView, context: Context) { adopt(into: container) }

  private func adopt(into container: UIView) {
    if webView.superview !== container {
      webView.removeFromSuperview()
      webView.frame = container.bounds
      webView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
      container.addSubview(webView)
    }
    apply(to: webView)
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
