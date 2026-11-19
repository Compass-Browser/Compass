// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassShared
import UIKit

/// Copying the page's link (Copy Link, ⌘⇧C, the address's Copy URL):
/// without tracking parameters unless Remove Tracking from Copied Links is
/// off (docs/design/ios.md, "Clean copied links").
enum CopyLink {
  /// Firefox's lists, bundled from third_party/strip_on_share/.
  static let rules = CleanLink(bundle: .main)

  static func copy(_ url: URL?) {
    guard let url else { return }
    UIPasteboard.general.url = cleaned(url)
  }

  static func cleaned(_ url: URL) -> URL {
    guard CleanLink.isOn(), let rules else { return url }
    return rules.clean(url)
  }
}
