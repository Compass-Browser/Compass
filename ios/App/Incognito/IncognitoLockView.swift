// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI

/// What covers incognito (docs/design/ios.md, "Incognito"): while the app
/// is inactive (so the app switcher's snapshot shows nothing), and once it
/// has been in the background with incognito tabs open, until Face ID or
/// the passcode unlocks it.
struct IncognitoLockView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.scenePhase) private var phase

  private var locked: Bool { app.services.incognitoLocked }

  var body: some View {
    ZStack {
      Rectangle().fill(.black).ignoresSafeArea()
      VStack(spacing: 14) {
        Image(systemName: "eyeglasses").font(.system(size: 44, weight: .semibold))
        if locked {
          Text("Incognito is locked").font(.headline)
          Button("Unlock") { unlock() }
            .buttonStyle(.borderedProminent)
            .tint(.white.opacity(0.25))
            .accessibilityIdentifier("UnlockIncognito")
          // Back to the other spaces without unlocking.
          Button("Show Other Tabs") {
            app.selectedNodeID = nil
            app.selectedSpaceID = app.store.spaces.first?.id
            app.showSidebar()
          }
          .font(.callout)
          .tint(.white.opacity(0.8))
        }
      }
      .foregroundStyle(.white)
    }
    .environment(\.colorScheme, .dark)
    .task(id: phase) { if phase == .active, locked { unlock() } }
  }

  private func unlock() {
    Task { await app.services.unlockIncognito() }
  }
}
