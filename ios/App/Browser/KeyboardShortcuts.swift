// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import SwiftUI
import WebKit

/// Keyboard shortcuts with a hardware keyboard (iPad, or an iPhone with
/// one), the desktop's where it has them (shortcuts.md): ⌘T and ⌘L the
/// command bar, ⌘O Peek's Open, ⌘S the sidebar, ⌃1–9 the spaces, ⌘⇧C copy the link, ⌘⇧T
/// reopen a closed tab.
/// iPadOS lists them in the menu bar and in the overlay ⌘ held down shows.
struct BrowserCommands: Commands {
  let services: AppServices
  @Environment(\.openWindow) private var openWindow

  /// The window the user is in.
  private var app: AppModel? { services.active }
  /// The page the user sees: a little window's (iPad), Peek's card's, else
  /// the shown tab's.
  private var webView: WKWebView? { peek?.page ?? app?.shownTab.flatMap { services.tabs.live[$0.id] } }
  /// The card or little window the user is in.
  private var peek: Peek? { services.keyLittle ?? app?.peek }

  var body: some Commands {
    CommandGroup(replacing: .newItem) {
      Button("New Tab") { app?.openCommandBar(newTab: true) }
        .keyboardShortcut("t")
      // iPad: another window, on the space this one shows.
      Button("New Window") { openWindow(value: WindowTarget()) }
        .keyboardShortcut("n")
      Button("New Incognito Tab") { app?.openIncognitoTab() }
        .keyboardShortcut("n", modifiers: [.command, .shift])
      Button("Open Location…") {
        // A little window's own address.
        if let peek {
          if peek.kind == .little { peek.editAddress += 1 }
        } else {
          app?.openCommandBar(newTab: false)
        }
      }
      .keyboardShortcut("l")
      // Peek's Open, as on the desktop.
      Button("Open Peeked Page") {
        if let little = services.keyLittle { little.openRequest += 1 } else { app?.openPeek() }
      }
      .keyboardShortcut("o")
      .disabled(peek == nil)
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
      // ⌃Tab, ⌃⇧Tab and ⌘W reach the app through BrowserKeys; these items
      // list them and act when picked.
      Button("Next Tab") { app?.stepEntry(1) }
        .keyboardShortcut(.tab, modifiers: .control)
      Button("Previous Tab") { app?.stepEntry(-1) }
        .keyboardShortcut(.tab, modifiers: [.control, .shift])
      Button("Close Tab") {
        if let little = services.keyLittle { little.closeRequest += 1 } else { app?.closeShown() }
      }
        .keyboardShortcut("w")
      // The desktop's Reopen Closed Tab: undoes the same stack as a shake.
      Button("Reopen Closed Tab") { app?.undoClose() }
        .keyboardShortcut("t", modifiers: [.command, .shift])
      Divider()
      ForEach(Array((app?.visibleSpaces ?? []).prefix(9).enumerated()), id: \.element.id) { index, space in
        Button(space.name.isEmpty ? "Space \(index + 1)" : space.name) { app?.selectSpace(at: index) }
          .keyboardShortcut(KeyEquivalent(Character("\(index + 1)")), modifiers: .control)
      }
    }
    // Web extensions' `commands` (their manifests' keyboard shortcuts):
    // the keys reach them through BrowserKeys; picked here, the same.
    CommandMenu("Extensions") {
      let shortcuts = services.extensions.shortcuts()
      if shortcuts.isEmpty {
        Button("No Extension Shortcuts") {}.disabled(true)
      }
      ForEach(shortcuts) { shortcut in
        Button(shortcut.title) { shortcut.perform() }
          .keyboardShortcut(shortcut.keyboardShortcut)
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

/// ⌘W, ⌃Tab, ⌃⇧Tab and extensions' shortcuts, taken before anything else
/// sees them. As SwiftUI commands or root view controller key commands they
/// didn't reliably reach the app: the system's Close Window keeps ⌘W, and a
/// focused page takes Tab (⌃Tab worked once). So the app's `sendEvent`
/// looks at hardware key presses first (`compassSendEvent`) and acts on
/// these in a browser window (not a little window, which has its own ⌘W).
/// The key's release is swallowed with its press.
@MainActor enum BrowserKeys {
  private static var installed = false
  /// Key codes whose press was taken: their release is too.
  private static var taken: Set<Int> = []
  static let modifiers: UIKeyModifierFlags = [.command, .control, .alternate, .shift]

  /// Called as each browser window comes up.
  static func install() {
    KeyboardWatch.shared.start()
    guard !installed else { return }
    installed = true
    let type: AnyClass = UIApplication.self
    guard let original = class_getInstanceMethod(type, #selector(UIApplication.sendEvent(_:))),
      let ours = class_getInstanceMethod(type, #selector(UIApplication.compassSendEvent(_:)))
    else { return }
    method_exchangeImplementations(original, ours)
  }

  /// Whether the event was the browser's (and goes no further).
  static func handle(_ event: UIPressesEvent) -> Bool {
    for press in event.allPresses {
      guard let key = press.key else { continue }
      let code = key.keyCode.rawValue
      switch press.phase {
      case .began:
        guard let action = action(for: key, in: press.window) else { continue }
        taken.insert(code)
        action()
        return true
      case .ended, .cancelled:
        if taken.remove(code) != nil { return true }
      default:
        if taken.contains(code) { return true }
      }
    }
    return false
  }

  /// What `key` does in `window` (the key window when the press has none),
  /// if it's one of these.
  private static func action(for key: UIKey, in window: UIWindow?) -> (@MainActor () -> Void)? {
    let flags = key.modifierFlags.intersection(modifiers)
    guard !flags.isEmpty, let services = AppServices.shared,
      let scene = (window ?? keyWindow)?.windowScene,
      let app = services.windows.first(where: { $0.windowScene === scene })
    else { return nil }
    let characters = key.charactersIgnoringModifiers.lowercased()
    // A sheet (Settings, the command bar, a popup) keeps its keys; Peek's
    // card closes with ⌘W.
    let sheet = scene.keyWindow?.rootViewController?.presentedViewController != nil
    let tab = key.keyCode == .keyboardTab
    if tab && (flags == .control || flags == [.control, .shift]) {
      guard !sheet else { return nil }
      return { app.stepEntry(flags.contains(.shift) ? -1 : 1) }
    }
    if characters == "w" && flags == .command {
      // Never the system's Close Window: with a sheet other than Peek's
      // up, nothing.
      return { if app.peek != nil || !sheet { app.closeShown() } }
    }
    if #available(iOS 18.4, *), !sheet, !isReserved(characters, flags),
      let command = services.extensions.command(key: characters, modifiers: flags)
    {
      return { services.extensions.perform(command) }
    }
    return nil
  }

  private static var keyWindow: UIWindow? {
    UIApplication.shared.connectedScenes.compactMap { ($0 as? UIWindowScene)?.keyWindow }.first
  }

  /// Keys the browser (BrowserCommands) and editing keep: an extension's
  /// shortcut with them is left out, as Chrome keeps its own.
  static func isReserved(_ characters: String, _ flags: UIKeyModifierFlags) -> Bool {
    guard characters.count == 1 else { return false }
    switch flags {
    case .command: return "tnlofsr[]wcvxazqh,".contains(characters)
    case [.command, .shift]: return "crtnez".contains(characters)
    case .control: return characters == "\t" || "123456789".contains(characters)
    case [.control, .shift]: return characters == "\t"
    default: return false
    }
  }
}

extension UIApplication {
  /// `sendEvent(_:)`, swapped in by `BrowserKeys.install()`: calling it
  /// here calls the system's.
  @objc dynamic func compassSendEvent(_ event: UIEvent) {
    if let presses = event as? UIPressesEvent, BrowserKeys.handle(presses) { return }
    compassSendEvent(event)
  }
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
