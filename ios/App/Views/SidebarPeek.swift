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

/// The page's dimming under the sidebar: one layer for both the swiped-up
/// copy and the sheet (which doesn't dim itself), so nothing changes when
/// one takes over from the other. Darkest with the sidebar at full height,
/// lighter at half height, gone as it closes. Tapping it closes the sidebar.
struct SidebarDim: View {
  @Environment(AppModel.self) private var app
  /// At full height: the system sheet's own dimming (measured: 238 → 190).
  static let full: CGFloat = 0.2

  var body: some View {
    let amount = Self.full * fraction
    Color.black.opacity(amount)
      .ignoresSafeArea()
      .allowsHitTesting(app.showingSidebar && amount > 0)
      .onTapGesture { app.showingSidebar = false }
  }

  /// How far up the sidebar is, 0...1, from the sheet if it's up, else
  /// from the swipe.
  private var fraction: CGFloat {
    if let top = app.sidebarSheetCurrentTop {
      let screen = (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.screen.bounds.height ?? 800
      let full = SidebarPeek.top(app)
      return min(max((screen - top) / max(screen - full, 1), 0), 1)
    }
    return app.sidebarPeek ?? 0
  }
}
