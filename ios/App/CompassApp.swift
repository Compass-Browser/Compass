// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import BackgroundTasks
import CompassCrypto
import SwiftUI

@main
struct CompassApp: App {
  @State private var app: AppModel
  @Environment(\.scenePhase) private var phase
  static let refreshTask = "it.cipi1965.compass.refresh"

  init() {
    let app = AppModel()
    _app = State(initialValue: app)
    // A best-effort sync while in the background (push needs the app open).
    BGTaskScheduler.shared.register(forTaskWithIdentifier: Self.refreshTask, using: nil) { task in
      let work = Task { @MainActor in
        await app.sync.syncNow()
        task.setTaskCompleted(success: true)
      }
      task.expirationHandler = { work.cancel() }
      Self.scheduleRefresh()
    }
  }

  static func scheduleRefresh() {
    let request = BGAppRefreshTaskRequest(identifier: refreshTask)
    request.earliestBeginDate = Date(timeIntervalSinceNow: 15 * 60)
    try? BGTaskScheduler.shared.submit(request)
  }

  var body: some Scene {
    WindowGroup {
      RootView()
        .environment(app)
        .onOpenURL { url in
          // compass-pair: links from the Mac's QR code (camera app).
          if let invite = PairingInvite(url: url), !app.sync.isOn {
            app.pendingInvite = invite
            app.showingSettings = true
          }
        }
    }
    .onChange(of: phase) { _, phase in
      app.sync.setActive(phase == .active)
      if phase == .background {
        try? app.store.save(to: app.sidebarURL)
        Self.scheduleRefresh()
      }
    }
  }
}

struct RootView: View {
  @Environment(AppModel.self) private var app

  var body: some View {
    @Bindable var app = app
    // The browser is always underneath; the sidebar is a panel over it
    // (SidebarPanel), and the command bar and Settings are sheets over both.
    BrowserScreen(node: app.selectedNode)
      .overlay { SidebarPanel() }
      .sheet(isPresented: $app.showingCommandBar) {
        // A new tab, or the shown tab's address.
        CommandBar(replacing: app.commandBarNewTab ? nil : app.shownTab)
      }
      .sheet(isPresented: $app.showingSettings) { SettingsView() }
  }
}
