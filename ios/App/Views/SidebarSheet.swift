// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI
import UIKit

/// Presents the sidebar as a bottom sheet (medium and large detents, the
/// grabber, drag down to close) through UIKit, following
/// AppModel.showingSidebar. Not SwiftUI's `.sheet`: after a swipe up
/// (SidebarPeek) the sheet has to appear with no animation, exactly where
/// the swiped-up copy stopped, and SwiftUI always animates a sheet in on
/// the device, so a second sidebar slid up over the first.
@MainActor
final class SidebarSheet: NSObject, UISheetPresentationControllerDelegate {
  private let app: AppModel
  private weak var controller: UIViewController?

  init(app: AppModel) {
    self.app = app
  }

  /// Present or dismiss to match the model. `instant`: the swipe-up
  /// handover, with no animation, after which the swiped-up copy goes.
  func sync(instant: Bool = false) {
    if app.showingSidebar {
      present(animated: !instant)
    } else if let controller {
      self.controller = nil
      controller.dismiss(animated: true) { [app] in
        if !app.showingSidebar { app.sidebarSheetCurrentTop = nil }
      }
    }
  }

  private func present(animated: Bool) {
    guard controller == nil else { return }
    guard let root = Self.rootController() else {
      // Before the window is up (launch): try again shortly.
      Task {
        try? await Task.sleep(for: .milliseconds(100))
        self.sync(instant: !animated)
      }
      return
    }
    let host = UIHostingController(rootView: SidebarSheetRoot().environment(app))
    // Opaque: nothing behind the content shows through (black) before
    // it has drawn.
    host.view.backgroundColor = .systemBackground
    host.modalPresentationStyle = .pageSheet
    if let sheet = host.sheetPresentationController {
      sheet.detents = [.medium(), .large()]
      sheet.selectedDetentIdentifier = .large
      sheet.prefersGrabberVisible = true
      // SidebarDim dims the page instead (one layer with the swiped-up
      // copy's), and taps on it close the sheet.
      sheet.largestUndimmedDetentIdentifier = .large
      sheet.preferredCornerRadius = SidebarPeek.radius
      sheet.delegate = self
    }
    controller = host
    var top = root
    while let presented = top.presentedViewController { top = presented }
    // Drawn before it's shown, so its first frame isn't empty.
    host.view.frame = top.view.bounds
    host.view.layoutIfNeeded()
    // The swiped-up copy stays under the sheet until it's in, so there's
    // always a sidebar on screen.
    top.present(host, animated: animated) { [app] in
      if app.sidebarPeek != nil { app.sidebarPeek = nil }
    }
  }

  /// Closed by dragging it down.
  func presentationControllerDidDismiss(_ presentationController: UIPresentationController) {
    controller = nil
    app.showingSidebar = false
    app.sidebarSheetCurrentTop = nil
  }

  private static func rootController() -> UIViewController? {
    UIApplication.shared.connectedScenes
      .compactMap { ($0 as? UIWindowScene)?.keyWindow }
      .first?.rootViewController
  }
}

/// The sheet's content: the sidebar on its space's tint, measuring where the
/// sheet rests at full height for SidebarPeek.
private struct SidebarSheetRoot: View {
  @Environment(AppModel.self) private var app

  var body: some View {
    SidebarScreen()
      .background { SpaceTint(color: app.selectedSpace?.color.color ?? .blue) }
      .onGeometryChange(for: CGFloat.self, of: { $0.frame(in: .global).minY }) { top in
        app.sidebarSheetCurrentTop = top
        // The sheet moves while it slides in or out, follows a drag, or
        // sits at half height; at full height it's at the highest point
        // it reaches.
        if top > 0, app.sidebarSheetTop == 0 || top < app.sidebarSheetTop - 0.5 {
          app.sidebarSheetTop = top
        }
      }
  }
}
