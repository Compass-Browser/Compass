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
  private let services: AppServices
  @Environment(\.scenePhase) private var phase
  static let refreshTask = "it.cipi1965.compass.refresh"

  init() {
    let services = AppServices.start()
    self.services = services
    // Tabs sent from other devices; a tap that launched the app arrives here.
    SentTabNotifications.shared.setUp(services: services)
    AutoPiP.configureAudioSession()
    // Leaving the app moves a playing video to picture-in-picture, from the
    // first moment iOS says so (scenePhase comes later).
    AutoPiP.watchLeavingTheApp { [weak services] in services?.active?.leavingAppVideo() }
    // A best-effort sync while in the background (push needs the app open).
    // On the main queue: the handler is written in the App's init, so Swift
    // takes it as main-actor code and traps if it runs anywhere else (with
    // `using: nil` the scheduler called it on a background queue, and the
    // app crashed in the background).
    BGTaskScheduler.shared.register(forTaskWithIdentifier: Self.refreshTask, using: .main) { task in
      let work = Task { @MainActor in
        await services.sync.syncNow()
        await services.contentBlocking.refresh()
        // Live folders that are due (each device runs its own plugins).
        await services.liveFolders.refreshDueAndWait()
        try? services.store.save(to: services.sidebarURL)
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
    // Every window (an iPad can have several) is one of these. Its value is
    // the entry it shows, so iPadOS restores each window on that entry; the
    // first window at launch has none.
    WindowGroup(for: WindowTarget.self) { $target in
      WindowRoot(target: $target)
    }
    .commands { BrowserCommands(services: services) }
    // iPad: a link from another app in a window of its own (Peek.swift).
    // Only the little-window activity opens one: the browser windows don't
    // claim it (no handlesExternalEvents(preferring:allowing:)).
    WindowGroup(id: "little") {
      LittleWindowScene()
    }
    .defaultSize(width: 720, height: 900)
    .handlesExternalEvents(matching: [LittleWindows.activityType])
    .onChange(of: phase) { old, phase in
      let app = services.active
      services.incognitoPhase(phase)
      services.sync.setActive(phase == .active)
      // Live folders refresh while the app is in use (and in the
      // background refresh task).
      services.liveFolders.setActive(phase == .active)
      // Plugins copied into Files › Compass › Live Folder Plugins meanwhile.
      if phase == .active, old != .active { services.liveFolders.reloadPlugins() }
      // Auto picture-in-picture: leaving the app starts it (AutoPiP, on
      // resigning active), coming back puts the video back in the page.
      if phase == .active, old != .active { app?.appActiveChanged(true) }
      // Filter lists: checked at most daily.
      if phase == .active {
        Task { await services.contentBlocking.refresh() }
        if services.sync.isOn { SentTabNotifications.shared.requestAuthorization() }
        // The share sheet's and Safari's requests; the Focus filter's space.
        services.system.sharedChanged()
      }
      if phase == .background {
        try? services.store.save(to: services.sidebarURL)
        services.system.writeSnapshot()
        Self.scheduleRefresh()
      }
    }
  }
}

/// What a window was opened for: an entry (Open in New Window, or the one
/// it showed when iPadOS saved it), or nothing (⌘N). `token` keeps two
/// windows apart: SwiftUI brings up an open window with the same value
/// instead of opening another.
struct WindowTarget: Hashable, Codable {
  var entry: String?
  var token = UUID()
}

/// An entry dragged out of a window (a sidebar row, a split pane's title
/// strip). Its drag item carries the entry's ID as text, for the sidebar's
/// drops, and on iPad an `NSUserActivity` too: dropped where no window
/// takes it (beside the window, on the screen's edge), iPadOS opens a new
/// window with the activity, which shows the entry, its page moving there
/// as with Open in New Window. The browser windows' WindowGroup gets the
/// scene (the activity has no target content identifier, so the little
/// windows' group doesn't match it); `AppDelegate` notes the entry for the
/// scene's session, and the window shows it once it knows its scene.
enum TabDrag {
  static let activityType = "it.cipi1965.compass.tab"
  static let entryKey = "entry"

  @MainActor static func provider(entry id: String, title: String, opensWindow: Bool) -> NSItemProvider {
    let provider = NSItemProvider(object: id as NSString)
    if opensWindow, UIApplication.shared.supportsMultipleScenes {
      let activity = NSUserActivity(activityType: activityType)
      activity.title = title
      activity.userInfo = [entryKey: id]
      provider.registerObject(activity, visibility: .all)
    }
    return provider
  }

  /// The entry a dragged-out activity names.
  static func entry(of activity: NSUserActivity) -> String? {
    activity.activityType == activityType ? activity.userInfo?[entryKey] as? String : nil
  }
}

/// One window: its own model (what it shows) over the app's services.
struct WindowRoot: View {
  @Binding var target: WindowTarget?
  @State private var app: AppModel?
  @Environment(\.openWindow) private var openWindow

  var body: some View {
    Group {
      if let app {
        RootView()
          .environment(app)
          .background {
            KeyWindowReader(window: { window in
              app.windowScene = window.windowScene
              // A window opened by dropping an entry dragged out of
              // another (TabDrag).
              if let session = window.windowScene?.session,
                let entry = AppServices.shared.takeDraggedOut(for: session.persistentIdentifier)
              {
                showDragged(entry, in: app)
              }
            }) { AppServices.shared.activate(app) }
          }
          .onContinueUserActivity(TabDrag.activityType) { activity in
            if let entry = TabDrag.entry(of: activity) { showDragged(entry, in: app) }
          }
          // Saved with the window: iPadOS restores it on this entry.
          .onChange(of: app.selectedNodeID) { _, id in
            // Not an incognito entry: iPadOS saves it to disk.
            let entry = id.flatMap { app.store.node(id: $0) }.map(app.store.isIncognito) == true ? nil : id
            target = WindowTarget(entry: entry, token: target?.token ?? UUID())
          }
          .onDisappear { app.detach() }
          .onOpenURL { url in open(url, in: app) }
          // A Spotlight result: a space, a favorite or a Pinned entry.
          .onContinueUserActivity(CSSearchableItemActionType) { activity in
            guard let id = activity.userInfo?[CSSearchableItemActivityIdentifier] as? String,
              id != Spotlight.appItemID
            else { return }
            if id.hasPrefix(Spotlight.spacePrefix) {
              app.system.handle(.space(String(id.dropFirst(Spotlight.spacePrefix.count))))
            } else {
              app.system.handle(.node(id))
            }
          }
      } else {
        Color.clear
      }
    }
    .onAppear {
      guard app == nil else { return }
      let model = AppModel(services: .shared)
      app = model
      model.attach(showing: target?.entry)
      // A little window's page opened in a space while no window was.
      if let pending = AppServices.shared.takePendingAdopt() { model.adopt(pending.peek, in: pending.space) }
      #if DEBUG
        // -CompassSecondWindow YES: the first window's entry, 8 s in, in a
        // window of its own (as "Open in New Window").
        if AppServices.shared.windows.count == 1, UserDefaults.standard.bool(forKey: "CompassSecondWindow") {
          Task {
            try? await Task.sleep(for: .seconds(8))
            openWindow(value: WindowTarget(entry: model.selectedNodeID))
          }
        }
      #endif
    }
  }

  /// An entry dragged out of another window: shown here, and its page
  /// moves here (the window is the key one).
  private func showDragged(_ entry: String, in app: AppModel) {
    guard let node = app.store.node(id: entry), !node.isFolder else { return }
    if app.selectedNode !== node, app.shownTab !== node { app.show(node) }
    AppServices.shared.activate(app)
  }

  private func open(_ url: URL, in app: AppModel) {
    // The window that got the link is the one to act in.
    AppServices.shared.activate(app)
    // compass: links from widgets, the icon menu and the extensions.
    if let link = DeepLink(url: url) {
      app.system.handle(link)
      return
    }
    // A web link, with Compass as the default browser: a little window
    // (the card, Peek.swift) over whatever the window shows, where Open in
    // <space> makes it a tab; with the setting off, a new tab of the shown
    // space.
    if let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https" {
      if LittleWindows.isOn { app.showLittleWindow(url) } else { app.openLink(url) }
      return
    }
    // compass-pair: links from the Mac's QR code (camera app).
    if let invite = PairingInvite(url: url), !app.sync.isOn {
      app.pendingInvite = invite
      app.showingSettings = true
    }
  }
}

/// Calls `becameKey` when this view's window becomes the key window (the
/// one the user is in), and at once if it already is.
struct KeyWindowReader: UIViewRepresentable {
  /// The browser's ⌘W, ⌃Tab and ⌃⇧Tab (BrowserKeys): not in a little
  /// window.
  var installsKeyCommands = true
  /// The window it's in, when that changes.
  var window: ((UIWindow) -> Void)? = nil
  let becameKey: () -> Void

  func makeUIView(context: Context) -> Reader {
    let reader = Reader(becameKey: becameKey)
    reader.installsKeyCommands = installsKeyCommands
    reader.onWindow = window
    return reader
  }

  func updateUIView(_ view: Reader, context: Context) {
    view.becameKey = becameKey
    view.onWindow = window
  }

  final class Reader: UIView {
    var becameKey: () -> Void
    var installsKeyCommands = true
    var onWindow: ((UIWindow) -> Void)?
    private var observer: NSObjectProtocol?

    init(becameKey: @escaping () -> Void) {
      self.becameKey = becameKey
      super.init(frame: .zero)
      isUserInteractionEnabled = false
    }

    required init?(coder: NSCoder) { fatalError() }

    override func didMoveToWindow() {
      super.didMoveToWindow()
      if let observer { NotificationCenter.default.removeObserver(observer) }
      observer = nil
      guard let window else { return }
      observer = NotificationCenter.default.addObserver(
        forName: UIWindow.didBecomeKeyNotification, object: window, queue: .main
      ) { [weak self] _ in MainActor.assumeIsolated { self?.becameKey() } }
      if window.isKeyWindow { DispatchQueue.main.async { [weak self] in self?.becameKey() } }
      onWindow?(window)
      if installsKeyCommands { BrowserKeys.install() }
    }
  }
}

/// The toast after a close: what closed, and Undo; after an undo, Redo.
struct ClosedToastView: View {
  @Environment(AppModel.self) private var app
  let toast: ClosedToast

  var body: some View {
    HStack(spacing: 14) {
      Text(toast.text)
        .font(.footnote.weight(.medium))
        .lineLimit(1)
        .truncationMode(.middle)
      Button(toast.redo ? "Redo" : "Undo") {
        withAnimation(.snappy) { app.closedToast = nil }
        if toast.redo { app.redoClose() } else { app.undoClose() }
      }
      .font(.footnote.weight(.semibold))
    }
    .padding(.horizontal, 16)
    .padding(.vertical, 10)
    .background(.regularMaterial, in: Capsule())
    .shadow(color: .black.opacity(0.12), radius: 8, y: 2)
    .padding(.horizontal, 24)
    // A flick down puts it away.
    .gesture(DragGesture(minimumDistance: 8).onEnded { value in
      if value.translation.height > 0 { withAnimation(.snappy) { app.closedToast = nil } }
    })
  }
}

struct RootView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.horizontalSizeClass) private var sizeClass
  /// The sidebar column's width (iPad), as the desktop's: dragged at its
  /// edge between these, kept for every window and the next launch; a
  /// double tap on the edge puts it back.
  static let columnWidth: CGFloat = 320
  static let columnWidths: ClosedRange<CGFloat> = 240...480
  /// The page keeps this much beside the column (a small Stage Manager
  /// window): the column gives way, down to its narrowest.
  static let minPageWidth: CGFloat = 400
  @AppStorage("SidebarColumnWidth") private var storedColumnWidth = Double(RootView.columnWidth)
  @Environment(\.layoutDirection) private var layoutDirection
  @State private var windowWidth: CGFloat = 0
  /// The column's width when the edge's drag started.
  @State private var resizeStart: CGFloat?

  /// The column's width in this window.
  private var columnWidth: CGFloat {
    Self.clampedColumnWidth(stored: CGFloat(storedColumnWidth), window: windowWidth)
  }

  static func clampedColumnWidth(stored: CGFloat, window: CGFloat) -> CGFloat {
    let widest = window > 0 ? min(columnWidths.upperBound, window - minPageWidth) : columnWidths.upperBound
    return max(columnWidths.lowerBound, min(stored, widest))
  }

  /// The column's trailing edge: dragged, it resizes the column; a double
  /// tap goes back to the default width.
  private var resizeEdge: some View {
    Color.clear
      .frame(width: 16)
      .contentShape(Rectangle())
      .offset(x: layoutDirection == .rightToLeft ? -8 : 8)
      .gesture(
        DragGesture(minimumDistance: 2, coordinateSpace: .global)
          .onChanged { value in
            let start = resizeStart ?? columnWidth
            resizeStart = start
            let delta = layoutDirection == .rightToLeft ? -value.translation.width : value.translation.width
            storedColumnWidth = Double(Self.clampedColumnWidth(stored: start + delta, window: windowWidth))
          }
          .onEnded { _ in resizeStart = nil }
      )
      .onTapGesture(count: 2) { storedColumnWidth = Double(Self.columnWidth) }
      .accessibilityElement()
      .accessibilityLabel("Sidebar width")
      .accessibilityIdentifier("SidebarResize")
      .accessibilityValue("\(Int(columnWidth)) points")
      .accessibilityAdjustableAction { direction in
        let step: CGFloat = direction == .increment ? 20 : -20
        storedColumnWidth = Double(Self.clampedColumnWidth(stored: columnWidth + step, window: windowWidth))
      }
  }

  var body: some View {
    @Bindable var app = app
    // The browser is always underneath; the sidebar and the tab picker are
    // panels over it (SidebarPanel, TabPicker), and the command bar and
    // Settings are sheets over them. In a wide window (iPad) the sidebar is
    // a column beside the browser instead, as on the desktop.
    HStack(spacing: 0) {
      if app.isWide, app.showingSidebarColumn {
        SidebarScreen()
          .frame(width: columnWidth)
          .background { SpaceTint(theme: app.selectedSpace?.theme ?? .blue).ignoresSafeArea() }
          .spaceAppearance(app.selectedSpace?.theme)
          .overlay(alignment: .trailing) { resizeEdge }
          // The edge's handle runs over the page's side.
          .zIndex(1)
          .transition(.move(edge: .leading))
      }
      BrowserScreen(node: app.selectedNode)
        // Incognito, locked or covered: its page and bar don't show.
        .overlay { if app.incognitoPageHidden { IncognitoLockView() } }
    }
    .onGeometryChange(for: CGFloat.self) { $0.size.width } action: { windowWidth = $0 }
    .animation(.snappy(duration: 0.3), value: app.showingSidebarColumn)
    // iPad only: a Max iPhone in landscape is regular width too, and keeps
    // the phone's layout.
    .onChange(of: sizeClass, initial: true) {
      app.isWide = UIDevice.current.userInterfaceIdiom == .pad && sizeClass == .regular
    }
      .overlay { if !app.isWide { SidebarPanel() } }
      .overlay { TabPicker() }
      .sheet(isPresented: $app.showingCommandBar) {
        // A new tab, or the shown tab's address.
        CommandBar(replacing: app.commandBarNewTab ? nil : app.shownTab)
      }
      .sheet(isPresented: $app.showingSettings) { SettingsView() }
      // Claude working with its sheet closed: a pill to open it or stop.
      .overlay(alignment: .bottom) {
        if app.claude.streaming && !app.showingClaude {
          HStack(spacing: 10) {
            Button { app.showingClaude = true } label: {
              Label("Claude is working…", systemImage: "sparkles").font(.footnote.weight(.medium))
            }
            Button("Stop") { app.claude.stop() }.font(.footnote.weight(.semibold))
          }
          .padding(.horizontal, 14)
          .padding(.vertical, 8)
          .background(.regularMaterial, in: Capsule())
          .padding(.bottom, 76)
          .transition(.move(edge: .bottom).combined(with: .opacity))
        }
      }
      .animation(.snappy, value: app.claude.streaming && !app.showingClaude)
      // A close's toast: Undo (or, after one, Redo), over everything but
      // the sheets.
      .overlay(alignment: .bottom) {
        if let toast = app.closedToast {
          ClosedToastView(toast: toast)
            .padding(.bottom, app.claude.streaming && !app.showingClaude ? 124 : 76)
            .transition(.move(edge: .bottom).combined(with: .opacity))
            .id(toast.id)
        }
      }
      // A shake, as iOS asks for typing: undo (or redo) closing.
      .alert(
        app.services.closed.canUndo ? "Undo Close Tab" : "Redo Close Tab",
        isPresented: $app.showingUndoAlert
      ) {
        Button("Cancel", role: .cancel) {}
        if app.services.closed.canRedo, app.services.closed.canUndo {
          Button("Redo") { app.redoClose() }
        }
        Button(app.services.closed.canUndo ? "Undo" : "Redo") {
          if app.services.closed.canUndo { app.undoClose() } else { app.redoClose() }
        }
      } message: {
        if let step = app.services.closed.canUndo ? app.services.closed.undoSteps.last : app.services.closed.redoSteps.last {
          Text(step.title)
        }
      }
      .sheet(isPresented: $app.showingClaude) { ClaudeChatSheet() }
      .sheet(isPresented: $app.showingMenu) { PageMenu() }
      .sheet(item: $app.liveEditor) { LiveFolderEditor(target: $0) }
      // Peek: a link looked at in a card (Peek.swift).
      .sheet(item: $app.peek) { peek in
        PeekCard(peek: peek, store: app.store, open: { app.openPeek(in: $0) }, close: { app.closePeek() })
      }
      .sheet(isPresented: $app.showingDownloads) {
        NavigationStack {
          DownloadsView().toolbar { Button("Done") { app.showingDownloads = false } }
        }
      }
  }
}
