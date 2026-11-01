// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI

/// The sidebar following a swipe up from the browser's bar
/// (AppModel.sidebarPeek). A sheet can't follow a finger, so this is a live
/// copy of it, laid out exactly where the sheet sits at full height and slid
/// down out of view by how far the swipe has to go. Letting go past halfway
/// hands over to the real sheet in the same place; before, it slides away.
struct SidebarPeek: View {
  @Environment(AppModel.self) private var app

  /// The sheet's dimming of the page at full height (measured: 238 → 190).
  static let dim: CGFloat = 0.2
  /// The sheet's corners, set on both so the handover doesn't show.
  static let radius: CGFloat = 32

  /// Where the sheet's top sits: as measured, or a guess before the first time.
  static func top(_ app: AppModel) -> CGFloat {
    if app.sidebarSheetTop > 0 { return app.sidebarSheetTop }
    let window = (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.keyWindow
    return window?.safeAreaInsets.top ?? 59
  }

  static var bottomInset: CGFloat {
    (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.keyWindow?.safeAreaInsets.bottom ?? 34
  }

  var body: some View {
    let progress = app.sidebarPeek ?? 0
    GeometryReader { geo in
      let top = Self.top(app)
      let height = geo.size.height - top
      ZStack(alignment: .top) {
        Color.black.opacity(app.sidebarHandingOver ? 0 : Self.dim * progress)
        SidebarScreen()
          // The sheet keeps its content above the home indicator; this
          // copy is laid out with the safe areas ignored, so it adds that.
          .padding(.bottom, Self.bottomInset)
          .frame(height: height)
          .background { SpaceTint(color: app.selectedSpace?.color.color ?? .blue) }
          .clipShape(UnevenRoundedRectangle(topLeadingRadius: Self.radius, topTrailingRadius: Self.radius))
          .overlay(alignment: .top) {
            Capsule().fill(Color.primary.opacity(0.22)).frame(width: 60, height: 5).padding(.top, 6)
          }
          // Laid out where the sheet will be (so its safe areas match),
          // moved down visually.
          .padding(.top, top)
          .offset(y: height * (1 - progress))
      }
    }
    .ignoresSafeArea()
    .allowsHitTesting(false)
  }
}
