// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI
import WidgetKit

/// Compass's widgets, its Control Center control and its download activity
/// (docs/design/ios.md, "System extensions"). They read the snapshot the
/// app writes to the App Group and never touch the network.
@main
struct CompassWidgets: WidgetBundle {
  var body: some Widget {
    SpaceWidget()
    TodayWidget()
    QuickWidget()
    DevicesWidget()
    DownloadActivityWidget()
    if #available(iOS 18.0, *) {
      NewTabControl()
    }
  }
}
