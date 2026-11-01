// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI

/// The sidebar: a panel over the browser, from the bottom up to the status
/// bar, with the page dimmed behind it. Not a system sheet, so a swipe up
/// on the browser's bar can move it with the finger, and the button, a
/// swipe, a drag and the code all move the one panel.
///
/// Its position (0 closed, 0.5 half, 1 full) is AppModel.sidebarDrag while
/// a finger moves it, else where it rests (showingSidebar, sidebarHalf).
/// It closes with a drag down on its top strip, a pull down on the list at
/// its top (iOS 18), or a tap on the dimmed page.
struct SidebarPanel: View {
  @Environment(AppModel.self) private var app

  static let radius: CGFloat = 32
  /// The page's dimming with the panel at full height.
  static let dim: CGFloat = 0.2
  /// A pull this far past the list's top (the scroll view's rubber band,
  /// about half the finger's travel) closes the panel.
  static let pullToClose: CGFloat = 50

  /// The panel's top at full height: just under the status bar.
  static var top: CGFloat { window?.safeAreaInsets.top ?? 59 }
  static var bottomInset: CGFloat { window?.safeAreaInsets.bottom ?? 34 }
  static var screenHeight: CGFloat { window?.screen.bounds.height ?? 800 }
  static var height: CGFloat { max(screenHeight - top, 1) }
  private static var window: UIWindow? {
    (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.keyWindow
  }

  var body: some View {
    let position = app.sidebarPosition
    let height = Self.height
    ZStack(alignment: .top) {
      Color.black.opacity(Self.dim * position)
        .allowsHitTesting(app.showingSidebar)
        .onTapGesture { app.showingSidebar = false }
      SidebarScreen()
        // Content above the home indicator, as a sheet keeps it; the panel
        // itself runs to the screen's bottom edge.
        .padding(.bottom, Self.bottomInset)
        .frame(height: height)
        .background { SpaceTint(color: app.selectedSpace?.color.color ?? .blue) }
        .clipShape(UnevenRoundedRectangle(topLeadingRadius: Self.radius, topTrailingRadius: Self.radius))
        .overlay(alignment: .top) { grabber }
        // Laid out at full height (so its safe areas are right), moved down
        // visually.
        .padding(.top, Self.top)
        .offset(y: height * (1 - position))
        .allowsHitTesting(app.showingSidebar && app.sidebarDrag == nil)
    }
    .ignoresSafeArea()
    .animation(.snappy(duration: 0.32), value: app.showingSidebar)
    .animation(.snappy(duration: 0.32), value: app.sidebarHalf)
  }

  /// The grab handle, on a strip across the panel's top that drags it.
  private var grabber: some View {
    Capsule().fill(Color.primary.opacity(0.22))
      .frame(width: 40, height: 5)
      .padding(.top, 7)
      .frame(maxWidth: .infinity, minHeight: 22, alignment: .top)
      .contentShape(Rectangle())
      .gesture(
        DragGesture(coordinateSpace: .global)
          .onChanged { value in
            app.sidebarDrag = clamp(app.sidebarRest - value.translation.height / Self.height)
          }
          .onEnded { value in
            let predicted = app.sidebarRest - value.predictedEndTranslation.height / Self.height
            app.settleSidebar(at: app.sidebarDrag ?? app.sidebarRest, predicted: predicted)
          })
  }
}

private func clamp(_ x: CGFloat) -> CGFloat { min(max(x, 0), 1) }

extension AppModel {
  /// Where the panel rests: 0 closed, 0.5 at half height, 1 full.
  var sidebarRest: CGFloat { showingSidebar ? (sidebarHalf ? 0.5 : 1) : 0 }

  /// Where it is now: under a finger, pulled down by the list, or resting.
  var sidebarPosition: CGFloat {
    if let drag = sidebarDrag { return clamp(drag) }
    guard showingSidebar else { return 0 }
    // Twice the rubber band, about the finger's travel; the list cancels its
    // own bounce (SpacePage), so list and panel move together.
    return clamp(sidebarRest - 2 * sidebarPull / SidebarPanel.height)
  }

  /// A finger let go at `position`, heading for `predicted` (both 0...1):
  /// the nearest stop to where it's heading, so a flick carries it.
  func settleSidebar(at position: CGFloat, predicted: CGFloat) {
    let target = [0, 0.5, 1].min { abs($0 - predicted) < abs($1 - predicted) } ?? 1
    withAnimation(.snappy(duration: 0.32)) {
      sidebarDrag = nil
      sidebarHalf = target == 0.5
      showingSidebar = target > 0
    }
  }
}

extension View {
  /// On the sidebar's list: pulling it down at its top moves the panel
  /// down with it (the list stays put in the panel), and letting go far
  /// enough closes it, as a sheet does. iOS 18; before, the top strip and
  /// a tap on the page close it.
  @ViewBuilder func sidebarPullToClose(_ app: AppModel) -> some View {
    if #available(iOS 18.0, *) {
      modifier(SidebarPull(app: app))
    } else {
      self
    }
  }
}

@available(iOS 18.0, *)
private struct SidebarPull: ViewModifier {
  let app: AppModel

  func body(content: Content) -> some View {
    content
      .onScrollGeometryChange(for: CGFloat.self) { geo in
        max(0, -(geo.contentOffset.y + geo.contentInsets.top))
      } action: { _, pull in
        // Always kept current (closed, it's ignored), so it's 0 next time.
        app.sidebarPull = pull
      }
      .onScrollPhaseChange { old, new in
        // Let go while pulled down far enough: close.
        if old == .interacting, new != .interacting, app.sidebarPull > SidebarPanel.pullToClose {
          withAnimation(.snappy(duration: 0.32)) {
            app.showingSidebar = false
            app.sidebarHalf = false
          }
        }
      }
  }
}
