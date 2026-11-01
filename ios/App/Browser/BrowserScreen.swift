// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI
import WebKit

/// M0: one web view and an address bar. Replaced by the sidebar and the
/// live-tab registry in M3.
struct BrowserScreen: View {
  @State private var webView = WKWebView()
  @State private var address = ""

  var body: some View {
    VStack(spacing: 0) {
      WebView(webView: webView)
        .ignoresSafeArea(edges: .top)
      HStack {
        Button { webView.goBack() } label: { Image(systemName: "chevron.left") }
        TextField("Search or enter address", text: $address)
          .textFieldStyle(.roundedBorder)
          .keyboardType(.webSearch)
          .textInputAutocapitalization(.never)
          .autocorrectionDisabled()
          .onSubmit { load(address) }
        Button { webView.goForward() } label: { Image(systemName: "chevron.right") }
      }
      .padding(8)
      .background(.bar)
    }
    .onAppear { load("https://example.com") }
  }

  private func load(_ text: String) {
    guard let url = URL(string: text.contains("://") ? text : "https://\(text)") else { return }
    webView.load(URLRequest(url: url))
  }
}
