// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI
import WebKit

/// Hosts an existing WKWebView. Web views are owned by the live-tab registry,
/// not by SwiftUI, so switching entries never reloads a page.
///
/// `bottomInset` is the floating chrome over the page's bottom edge: the page
/// scrolls under it, and its end still scrolls clear of it.
struct WebView: UIViewRepresentable {
  let webView: WKWebView
  var bottomInset: CGFloat = 0

  func makeUIView(context: Context) -> WKWebView {
    apply(to: webView)
    return webView
  }

  func updateUIView(_ uiView: WKWebView, context: Context) { apply(to: uiView) }

  private func apply(to view: WKWebView) {
    let scroll = view.scrollView
    guard scroll.contentInset.bottom != bottomInset else { return }
    scroll.contentInset.bottom = bottomInset
    scroll.verticalScrollIndicatorInsets.bottom = bottomInset
  }
}
