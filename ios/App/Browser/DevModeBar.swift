// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI
import UIKit

/// Development mode's bar over a page on one of its sites
/// (docs/design/dev-mode.md, "iOS"): the full URL, selectable, and Copy,
/// which copies it exactly as shown (no tracking parameters taken out, unlike
/// Copy Link).
struct DevModeBar: View {
  static let height: CGFloat = 32

  let url: String
  @State private var copied = false

  var body: some View {
    HStack(spacing: 8) {
      Image(systemName: "chevron.left.forwardslash.chevron.right")
        .font(.caption2.weight(.semibold))
        .foregroundStyle(.secondary)
        .accessibilityHidden(true)
      Text(url)
        .font(.caption.monospaced())
        .lineLimit(1)
        .truncationMode(.middle)
        .textSelection(.enabled)
        .frame(maxWidth: .infinity, alignment: .leading)
        .accessibilityLabel("Page address")
        .accessibilityValue(url)
      Button {
        guard let link = URL(string: url) else { return }
        UIPasteboard.general.url = link
        copied = true
        Task {
          try? await Task.sleep(for: .seconds(1.2))
          copied = false
        }
      } label: {
        Image(systemName: copied ? "checkmark" : "doc.on.doc")
          .font(.footnote)
          .frame(width: 32, height: Self.height)
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .accessibilityLabel("Copy URL")
      .accessibilityValue(copied ? "Copied" : "")
    }
    .padding(.leading, 10)
    .frame(height: Self.height)
    .background(.bar)
    .overlay(alignment: .bottom) { Divider() }
    .accessibilityIdentifier("DevModeBar")
  }
}
