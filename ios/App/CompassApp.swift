// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import BackgroundTasks
import CompassCrypto
import CompassShared
import CoreSpotlight
import SwiftUI

@main
struct CompassApp: App {
  @UIApplicationDelegateAdaptor(AppDelegate.self) private var delegate
  @State private var app: AppModel
  @Environment(\.scenePhase) private var phase
  static let refreshTask = "it.cipi1965.compass.refresh"

  init() {
    let app = AppModel()
    _app = State(initialValue: app)
    AppDelegate.app = app
    // Tabs sent from other devices; a tap that launched the app arrives here.
    SentTabNotifications.shared.setUp(app: app)
    AutoPiP.configureAudioSession()
    // A best-effort sync while in the background (push needs the app open).
    BGTaskScheduler.shared.register(forTaskWithIdentifier: Self.refreshTask, using: nil) { task in
      let work = Task { @MainActor in
        await app.sync.syncNow()
        await app.contentBlocking.refresh()
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
          // compass: links from widgets, the icon menu and the extensions.
          if let link = DeepLink(url: url) {
            app.system.handle(link)
            return
          }
          // A web link, with Compass as the default browser: straight to
          // the page, in a new tab of the shown space.
          if let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https" {
            app.openLink(url)
            return
          }
          // compass-pair: links from the Mac's QR code (camera app).
          if let invite = PairingInvite(url: url), !app.sync.isOn {
            app.pendingInvite = invite
            app.showingSettings = true
          }
        }
        // A Spotlight result: a favorite or Pinned entry.
        .onContinueUserActivity(CSSearchableItemActionType) { activity in
          if let id = activity.userInfo?[CSSearchableItemActivityIdentifier] as? String {
            app.system.handle(.node(id))
          }
        }
    }
    .commands { BrowserCommands(app: app) }
    .onChange(of: phase) { old, phase in
      app.sync.setActive(phase == .active)
      // Auto picture-in-picture: it has to start while the app is still on
      // screen, so on the way out (inactive), not in the background.
      if old == .active, phase == .inactive { app.appActiveChanged(false) }
      if phase == .active, old != .active { app.appActiveChanged(true) }
      // Filter lists: checked at most daily.
      if phase == .active {
        Task { await app.contentBlocking.refresh() }
        if app.sync.isOn { SentTabNotifications.shared.requestAuthorization() }
        // The share sheet's and Safari's requests; the Focus filter's space.
        app.system.sharedChanged()
      }
      if phase == .background {
        try? app.store.save(to: app.sidebarURL)
        app.system.writeSnapshot()
        Self.scheduleRefresh()
      }
    }
  }
}

struct RootView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.horizontalSizeClass) private var sizeClass
  static let columnWidth: CGFloat = 320

  var body: some View {
    @Bindable var app = app
    // The browser is always underneath; the sidebar and the tab picker are
    // panels over it (SidebarPanel, TabPicker), and the command bar and
    // Settings are sheets over them. In a wide window (iPad) the sidebar is
    // a column beside the browser instead, as on the desktop.
    HStack(spacing: 0) {
      if app.isWide, app.showingSidebarColumn {
        SidebarScreen()
          .frame(width: Self.columnWidth)
          .background { SpaceTint(theme: app.selectedSpace?.theme ?? .blue).ignoresSafeArea() }
          .spaceAppearance(app.selectedSpace?.theme)
          .transition(.move(edge: .leading))
      }
      BrowserScreen(node: app.selectedNode)
    }
    .animation(.snappy(duration: 0.3), value: app.showingSidebarColumn)
    // iPad only: a Max iPhone in landscape is regular width too, and keeps
    // the phone's layout.
    .onChange(of: sizeClass, initial: true) {
      app.isWide = UIDevice.current.userInterfaceIdiom == .pad && sizeClass == .regular
    }
      .overlay { if !app.isWide { SidebarPanel() } }
      // ⌘W and ⌃⇧Tab (KeyboardShortcuts.swift), once there's a window.
      .task {
        for _ in 0..<20 {
          if let window = (UIApplication.shared.connectedScenes.first as? UIWindowScene)?.keyWindow {
            PriorityKeyCommands.install(in: window, app: app)
            return
          }
          try? await Task.sleep(for: .milliseconds(250))
        }
      }
      .overlay { TabPicker() }
      .sheet(isPresented: $app.showingCommandBar) {
        // A new tab, or the shown tab's address.
        CommandBar(replacing: app.commandBarNewTab ? nil : app.shownTab)
      }
      .sheet(isPresented: $app.showingSettings) { SettingsView() }
      .sheet(isPresented: $app.showingMenu) { PageMenu() }
      .sheet(isPresented: $app.showingDownloads) {
        NavigationStack {
          DownloadsView().toolbar { Button("Done") { app.showingDownloads = false } }
        }
      }
  }
}
