// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import WebKit

/// Applies the enabled Boosts (docs/design/boosts.md, "iOS"): every
/// non-Incognito web view's content controller gets one WKUserScript per
/// Boost's CSS (document start, every frame, the "CompassBoosts" world)
/// and per Boost's JavaScript (document end, every frame, that world or
/// the page's own). WKUserScript has no URL matching, so each script
/// checks the frame's location first (BoostScripts).
///
/// When the Boosts change, every attached controller's Boost scripts are
/// swapped for the new ones; pages get them from their next load (Claude's
/// save_boost says so with needs_reload).
@MainActor
final class BoostInjector {
  static let shared = BoostInjector()

  static let world = WKContentWorld.world(name: BoostScripts.worldName)

  private let controllers = NSHashTable<WKUserContentController>.weakObjects()
  private var scripts: [WKUserScript]

  private init() {
    scripts = Self.scripts(for: BoostStore.shared.boosts)
  }

  /// A new web view's controller (not Incognito's): it gets the Boosts
  /// now and their changes later.
  func attach(_ controller: WKUserContentController) {
    guard !controllers.contains(controller) else { return }
    controllers.add(controller)
    scripts.forEach(controller.addUserScript)
  }

  /// The Boosts changed: every attached controller's scripts follow.
  func update(_ boosts: [Boost]) {
    let old = scripts
    scripts = Self.scripts(for: boosts)
    for controller in controllers.allObjects {
      // Scripts can't be removed one by one (nor by world): all go, and
      // the others come back.
      let others = controller.userScripts.filter { script in !old.contains { $0 === script } }
      controller.removeAllUserScripts()
      others.forEach(controller.addUserScript)
      scripts.forEach(controller.addUserScript)
    }
  }

  private static func scripts(for boosts: [Boost]) -> [WKUserScript] {
    boosts.filter(\.enabled).flatMap { boost -> [WKUserScript] in
      var result: [WKUserScript] = []
      if let css = BoostScripts.css(boost) {
        result.append(WKUserScript(source: css, injectionTime: .atDocumentStart, forMainFrameOnly: false, in: world))
      }
      if let js = BoostScripts.js(boost) {
        result.append(
          WKUserScript(
            source: js, injectionTime: .atDocumentEnd, forMainFrameOnly: false,
            in: boost.jsWorld == .main ? .page : world))
      }
      return result
    }
  }
}
