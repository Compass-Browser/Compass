// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI
import WebKit

/// Keyboard shortcuts with a hardware keyboard (iPad, or an iPhone with
/// one), the desktop's where it has them (shortcuts.md): ⌘T and ⌘L the
/// command bar, ⌘S the sidebar, ⌃1–9 the spaces, ⌘⇧C copy the link, ⌘⇧T
/// reopen a closed tab.
/// iPadOS lists them in the menu bar and in the overlay ⌘ held down shows.
struct BrowserCommands: Commands {
  let services: AppServices
  @Environment(\.openWindow) private var openWindow

  /// The window the user is in.
  private var app: AppModel? { services.active }
  private var webView: WKWebView? { app?.shownTab.flatMap { services.tabs.live[$0.id] } }

  var body: some Commands {
    CommandGroup(replacing: .newItem) {
      Button("New Tab") { app?.openCommandBar(newTab: true) }
        .keyboardShortcut("t")
      // iPad: another window, on the space this one shows.
      Button("New Window") { openWindow(value: WindowTarget()) }
        .keyboardShortcut("n")
      Button("New Incognito Tab") { app?.openIncognitoTab() }
        .keyboardShortcut("n", modifiers: [.command, .shift])
      Button("Open Location…") { app?.openCommandBar(newTab: false) }
        .keyboardShortcut("l")
    }
    CommandGroup(after: .pasteboard) {
      Button("Copy Link") {
        if let url = webView?.url { UIPasteboard.general.url = url }
      }
      .keyboardShortcut("c", modifiers: [.command, .shift])
      Button("Find…") { app?.findOnPage() }
        .keyboardShortcut("f")
      // The desktop's default for the Claude panel.
      Button("Ask Claude") { if app?.isIncognito != true { app?.showingClaude.toggle() } }
        .keyboardShortcut("e", modifiers: [.command, .shift])
    }
    CommandGroup(before: .sidebar) {
      Button(app.map { $0.isWide && $0.showingSidebarColumn } == true ? "Hide Sidebar" : "Show Sidebar") { toggleSidebar() }
        .keyboardShortcut("s")
      Button("Reload Page") { webView?.reload() }
        .keyboardShortcut("r")
      Button("Reload Without Cache") { webView?.reloadFromOrigin() }
        .keyboardShortcut("r", modifiers: [.command, .shift])
    }
    CommandMenu("History") {
      Button("Back") { webView?.goBack() }
        .keyboardShortcut("[")
      Button("Forward") { webView?.goForward() }
        .keyboardShortcut("]")
    }
    CommandMenu("Tabs") {
      Button("Next Tab") { app?.stepEntry(1) }
        .keyboardShortcut(.tab, modifiers: .control)
      // The desktop's Reopen Closed Tab: undoes the same stack as a shake.
      Button("Reopen Closed Tab") { app?.undoClose() }
        .keyboardShortcut("t", modifiers: [.command, .shift])
      Divider()
      ForEach(Array((app?.visibleSpaces ?? []).prefix(9).enumerated()), id: \.element.id) { index, space in
        Button(space.name.isEmpty ? "Space \(index + 1)" : space.name) { app?.selectSpace(at: index) }
          .keyboardShortcut(KeyEquivalent(Character("\(index + 1)")), modifiers: .control)
      }
    }
  }

  /// ⌘S: the column on an iPad; on a phone (or a narrow window) the bottom
  /// panel.
  private func toggleSidebar() {
    guard let app else { return }
    if app.isWide {
      app.showingSidebarColumn.toggle()
    } else if app.showingSidebar {
      app.showingSidebar = false
    } else {
      app.showSidebar()
    }
  }
}

/// ⌘W and ⌃⇧Tab, which the system keeps from SwiftUI's commands: UIKit key
/// commands that ask for priority over system behavior, on the window's root
/// view controller (in every responder chain, the page's included).
enum PriorityKeyCommands {
  @MainActor static func install(in window: UIWindow) {
    KeyboardWatch.shared.start()
    guard let root = window.rootViewController, root.keyCommands?.contains(where: { $0.action == #selector(UIViewController.compassCloseTab) }) != true
    else { return }
    let close = UIKeyCommand(title: "Close Tab", action: #selector(UIViewController.compassCloseTab), input: "w", modifierFlags: .command)
    let previous = UIKeyCommand(
      title: "Previous Tab", action: #selector(UIViewController.compassPreviousTab), input: "\t", modifierFlags: [.control, .shift])
    for command in [close, previous] {
      command.wantsPriorityOverSystemBehavior = true
      root.addKeyCommand(command)
    }
  }
}

extension UIViewController {
  @objc func compassCloseTab() { AppDelegate.app?.closeShown() }
  @objc func compassPreviousTab() { AppDelegate.app?.stepEntry(-1) }
}

/// Shake to undo closing (docs/design/ios.md, "Undo closing"): a shake asks
/// the window shown to undo, as iOS asks for typing. While the keyboard is
/// up the shake is the text's, iOS's own Undo Typing.
extension UIWindow {
  open override func motionEnded(_ motion: UIEvent.EventSubtype, with event: UIEvent?) {
    guard motion == .motionShake, !KeyboardWatch.shared.isUp, let app = AppDelegate.app else {
      return super.motionEnded(motion, with: event)
    }
    app.shaken()
  }
}

/// Whether the software keyboard is on screen.
@MainActor final class KeyboardWatch {
  static let shared = KeyboardWatch()
  private(set) var isUp = false
  private var observers: [NSObjectProtocol] = []

  /// Called at launch, before any keyboard can show.
  func start() {
    guard observers.isEmpty else { return }
    let center = NotificationCenter.default
    observers = [
      center.addObserver(forName: UIResponder.keyboardDidShowNotification, object: nil, queue: .main) { _ in
        MainActor.assumeIsolated { KeyboardWatch.shared.isUp = true }
      },
      center.addObserver(forName: UIResponder.keyboardWillHideNotification, object: nil, queue: .main) { _ in
        MainActor.assumeIsolated { KeyboardWatch.shared.isUp = false }
      },
    ]
  }
}
