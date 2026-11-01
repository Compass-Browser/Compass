// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import SwiftUI

@main
struct CompassApp: App {
  @State private var app = AppModel()
  @Environment(\.scenePhase) private var phase

  var body: some Scene {
    WindowGroup {
      RootView()
        .environment(app)
        .onOpenURL { url in
          // compass-pair: links from the Mac's QR code (camera app).
          if PairingInvite(url: url) != nil { app.showingSettings = true }
        }
    }
    .onChange(of: phase) { _, phase in
      if phase == .active { app.sync.syncSoon() }
      if phase == .background { try? app.store.save(to: app.sidebarURL) }
    }
  }
}

struct RootView: View {
  @Environment(AppModel.self) private var app

  var body: some View {
    @Bindable var app = app
    ZStack {
      if let node = app.selectedNode {
        BrowserScreen(node: node)
          .transition(.move(edge: .trailing))
      } else {
        SidebarScreen()
          .transition(.move(edge: .leading))
      }
    }
    .animation(.easeInOut(duration: 0.22), value: app.selectedNodeID)
    .sheet(isPresented: $app.showingCommandBar) {
      CommandBar(replacing: app.selectedNode.flatMap { $0.isTab ? $0 : nil })
    }
    .sheet(isPresented: $app.showingSettings) { SettingsView() }
  }
}
