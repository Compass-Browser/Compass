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
  @State private var sidebarDetent: PresentationDetent = .large

  var body: some View {
    @Bindable var app = app
    // The browser is always underneath; the sidebar is a bottom sheet over
    // it (swipe down to close, or to half height to see the page).
    BrowserScreen(node: app.selectedNode)
      .sheet(isPresented: $app.showingSidebar) {
        SidebarScreen()
          .presentationDetents([.medium, .large], selection: $sidebarDetent)
          .presentationDragIndicator(.visible)
      }
      .onChange(of: app.showingSidebar) { _, showing in
        if showing { sidebarDetent = .large }
      }
      // While the sidebar is up, it presents these itself (a sheet can't
      // present over another from the same view).
      .sheet(isPresented: app.presented(\.showingCommandBar, fromSidebar: false)) {
        CommandBar(replacing: app.selectedNode.flatMap { $0.isTab ? $0 : nil })
      }
      .sheet(isPresented: app.presented(\.showingSettings, fromSidebar: false)) { SettingsView() }
  }
}

extension AppModel {
  /// A sheet flag, for the view that presents it now: the sidebar while it's
  /// up, the browser otherwise.
  func presented(_ flag: ReferenceWritableKeyPath<AppModel, Bool>, fromSidebar: Bool) -> Binding<Bool> {
    Binding(
      get: { self[keyPath: flag] && self.showingSidebar == fromSidebar },
      set: { self[keyPath: flag] = $0 })
  }
}
