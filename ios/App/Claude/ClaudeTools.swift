// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import CompassModel
import UIKit
import WebKit

/// Runs Claude's tools on iOS (I3, docs/design/claude.md "iOS"): the shared
/// tools.json, with results shaped as the desktop's ClaudeTools gives them.
/// Page tools act on the chat's target tab:
/// - page_agent.js in the "claude" content world reads the page and acts
///   with synthetic DOM events (isTrusted == false: a few sites ignore
///   them; the real touch input is untouched);
/// - `takeSnapshot` takes screenshots (JPEG, at most 1280 px on a side);
/// - a document-start script in the page world keeps the console
///   (`ConsoleCapture`), for read_console;
/// - run_js runs in the page world.
/// Browser tools (other tabs, the sidebar, history, downloads, other
/// devices) need the whole-browser permission; the sidebar's are
/// CompassClaude's SidebarTools, as the desktop's ClaudeTabs.
@MainActor
final class ClaudeTools {
  struct Result {
    var content: [JSONValue]
    var isError = false
    /// What a memory tool changed, for the chat's Undo; not sent.
    var memoryChange: MemoryChange?

    static func text(_ text: String) -> Result { Result(content: [["type": "text", "text": .string(text)]]) }
    static func error(_ text: String) -> Result { Result(content: [["type": "text", "text": .string(text)]], isError: true) }
  }

  static let notPermitted =
    "Not permitted: the user hasn't turned on \"Let Claude use the whole browser\" (Compass settings, Claude). Without it, Claude can only use the tab the chat is about. Tell the user if the task needs more."
  /// The desktop's kDevToolsOff.
  static let devtoolsOff =
    "Not permitted: the user hasn't turned on \"Let Claude use developer tools\" (Compass settings, Claude). Tell the user if the task needs it."
  static let screenshotMaxEdge: CGFloat = 1280
  static let maxJSChars = 20000

  /// The tab the page tools act on: the chat's tab when a message is
  /// sent, or one Claude opened or selected since.
  var targetTab: String? {
    didSet {
      if targetTab != oldValue {
        screenshotScale = 1
        // Emulation is the tab's while Claude works on it.
        endEmulation()
      }
    }
  }
  /// The chat's tab at the last message (close_tab goes back to it).
  var taskTab: String?
  /// Tabs Claude opened in this chat: the only ones it may close.
  private var opened: Set<String> = []
  /// Screenshot pixels per CSS pixel, from the latest screenshot.
  private var screenshotScale: Double = 1
  /// read_console's "only new": the console entries already read.
  private var consoleRead = 0
  /// network_requests' "only new" (ClaudeDevTools.swift).
  var networkSeen = NetworkListing.Seen()
  /// What emulate changed, until it's undone (ClaudeDevTools.swift).
  var emulation: Emulation?
  var emulationEnd: Task<Void, Never>?

  private(set) weak var app: AppModel?

  /// What the user is asked before a file is written (docs/design/claude.md,
  /// "Files").
  struct Approval {
    /// The tool asking: "Allow for This Chat" allows it.
    var tool: String
    var title: String
    /// The content's start (ClaudeFiles.preview).
    var detail: String
  }

  /// Asks the user (the chat's approval card); without it, nothing is
  /// written.
  var approve: ((Approval) async -> Bool)?

  init(app: AppModel) { self.app = app }

  func reset() {
    screenshotScale = 1
    consoleRead = 0
    networkSeen = NetworkListing.Seen()
  }

  /// A new chat: Claude's own tabs stay, but aren't its to close any more.
  func forget() {
    reset()
    endEmulation()
    opened = []
  }

  /// What a memory tool did: "saved", "updated" or "forgot", the text now
  /// (forgot: what it was), and the memory before (none if new).
  struct MemoryChange {
    var kind: String
    var id: String
    var text: String
    var before: MemoryEntry?
  }

  /// Runs one tool call.
  func run(_ name: String, input: JSONValue) async -> Result {
    // Emulation ends a minute after Claude's last tool.
    defer { scheduleEmulationEnd() }
    let scopes = Self.scopes
    guard let scope = scopes[name] else { return .error("Unknown tool: \(name).") }
    if scope == "memory" { return runMemoryTool(name, input: input) }
    if scope == "files" { return await runFileTool(name, input: input) }
    if scope == "devtools" {
      guard ClaudeSettings.shared.devtools else { return .error(Self.devtoolsOff) }
    }
    if scope == "browser" {
      guard ClaudeSettings.shared.browserScope else { return .error(Self.notPermitted) }
      return await runBrowserTool(name, input: input)
    }
    guard let app, let id = targetTab, let node = app.store.node(id: id) else {
      return .error("The tab the chat is about was closed.")
    }
    guard !app.store.isIncognito(node) else { return .error("Claude can't use Incognito tabs.") }
    if name == "wait" {
      let seconds = min(max(input["seconds"]?.number ?? 1, 0.1), 10)
      try? await Task.sleep(for: .seconds(seconds))
      return .text("Waited \(seconds) s.")
    }
    if name == "navigate" { return await navigate(node, input: input) }
    // A tab out of every window is throttled by WebKit and can't be
    // snapshotted: it's kept rendering while Claude works on it.
    let wasLoaded = app.tabs.live[id] != nil
    let view = app.tabs.webView(for: node)
    Self.keepRendering(view)
    if !wasLoaded { await Self.waitForLoad(view) }
    guard let url = view.url, ["http", "https"].contains(url.scheme?.lowercased() ?? "") else {
      return .error("Claude can't use the browser's own pages, only web pages.")
    }
    switch name {
    case "read_page": return await agent(view, "readPage", input)
    case "get_page_text": return await agent(view, "pageText", input)
    case "find": return await agent(view, "find", input)
    case "click": return await acted(view, await agent(view, "click", cssInput(input)))
    case "hover": return await acted(view, await agent(view, "hover", cssInput(input)))
    case "type": return await agent(view, "type", input)
    case "key": return await agent(view, "key", input)
    case "scroll": return await agent(view, "scroll", cssInput(input))
    case "form_input": return await agent(view, "formInput", input)
    case "screenshot": return await screenshot(view, input: input)
    case "read_console": return await readConsole(view, input: input)
    case "run_js": return await runJS(view, input: input)
    case "network_requests", "network_response", "inspect_element", "page_storage", "edit_page_storage",
      "performance", "emulate":
      return await runDevTool(name, node: node, view: view, input: input)
    default: return .error("\(name) isn't available on iOS.")
    }
  }

  /// save_file, list_files, read_file, write_file: no tab needed; the Files
  /// setting decides, and writes ask the user first (the desktop's
  /// ClaudeTools::RunFileTool()).
  private func runFileTool(_ name: String, input: JSONValue) async -> Result {
    let settings = ClaudeSettings.shared
    let access = settings.fileAccess
    if access == .off { return .error(ClaudeFiles.notPermittedOff) }
    if access == .save && name != "save_file" { return .error(ClaudeFiles.notPermittedSaveOnly) }
    guard let folder = settings.openFolder(for: access) else { return .error(ClaudeFiles.notPermittedNoFolder) }
    defer { folder.close() }
    let files = ClaudeFiles(root: folder.url)
    let path = input["path"]?.string ?? ""
    let content = input["content"]?.string ?? ""
    do {
      switch name {
      case "save_file":
        let fileName = input["name"]?.string ?? ""
        guard ClaudeFiles.isValidName(fileName) else { return .error(ClaudeFiles.Failure.badName.description) }
        guard content.count <= ClaudeFiles.maxChars else { return .error(ClaudeFiles.Failure.tooLong.description) }
        let approval = Approval(
          tool: name, title: "Save “\(fileName)” in \(folder.name)", detail: ClaudeFiles.preview(content))
        guard await approve?(approval) == true else { return .error(ClaudeFiles.denied) }
        let url = try files.save(name: fileName, content: content)
        return .text("Saved as “\(url.lastPathComponent)” in \(folder.name).")
      case "list_files":
        return .text(try files.list(path))
      case "read_file":
        return .text(try files.read(path, offset: Int(input["offset"]?.number ?? 0)))
      case "write_file":
        guard content.count <= ClaudeFiles.maxChars else { return .error(ClaudeFiles.Failure.tooLong.description) }
        let target = try files.target(path)
        let approval = Approval(
          tool: name, title: "\(target.replaces ? "Replace" : "Create") “\(path)” in \(folder.name)",
          detail: ClaudeFiles.preview(content))
        guard await approve?(approval) == true else { return .error(ClaudeFiles.denied) }
        try files.write(content, to: target.url)
        return .text("\(target.replaces ? "Replaced" : "Wrote") “\(path)” (\(content.count) characters).")
      default:
        return .error("\(name) isn't available on iOS.")
      }
    } catch {
      return .error(String(describing: error))
    }
  }

  /// remember, update_memory, forget: the profile's memory, no tab needed
  /// (the desktop's ClaudeTools::RunMemoryTool()).
  private func runMemoryTool(_ name: String, input: JSONValue) -> Result {
    let settings = ClaudeSettings.shared
    guard settings.memoryEnabled && settings.memorySave else {
      return .error("The user turned off \"Let Claude Save Memories\" in Compass's Claude settings.")
    }
    let memory = ClaudeMemoryStore.shared
    let text = input["text"]?.string ?? ""
    let id = input["id"]?.string ?? ""
    if name == "remember" {
      let count = memory.entries.count
      switch memory.add(text) {
      case .failure(let failure): return .error(failure.description)
      case .success(let entry):
        let short = String(entry.id.prefix(8))
        if memory.entries.count == count { return .text("Already remembered, as [\(short)].") }
        var result = Result.text("Saved, as [\(short)].")
        result.memoryChange = MemoryChange(kind: "saved", id: entry.id, text: entry.text, before: nil)
        return result
      }
    }
    guard let before = memory.find(id) else {
      return .error("No memory has the id \(id). It may have been deleted since the chat started.")
    }
    if name == "update_memory" {
      switch memory.update(before.id, text: text) {
      case .failure(let failure): return .error(failure.description)
      case .success(let entry):
        var result = Result.text("Updated.")
        result.memoryChange = MemoryChange(kind: "updated", id: entry.id, text: entry.text, before: before)
        return result
      }
    }
    memory.remove(before.id)
    var result = Result.text("Forgotten.")
    result.memoryChange = MemoryChange(kind: "forgot", id: before.id, text: before.text, before: before)
    return result
  }

  /// tools.json's scopes, by tool name.
  static let scopes: [String: String] = {
    guard let url = Bundle.main.url(forResource: "tools", withExtension: "json"),
      let data = try? Data(contentsOf: url), let list = JSONValue.parse(data)?["tools"]?.array
    else { return [:] }
    var scopes: [String: String] = [:]
    for entry in list where ClaudeSettings.isForIOS(entry) {
      if let name = entry["definition"]?["name"]?.string { scopes[name] = entry["scope"]?.string ?? "page" }
    }
    return scopes
  }()

  // MARK: Tabs (browser scope)

  /// The sidebar tools (CompassClaude's SidebarTools) for this window.
  private func sidebarTools(_ app: AppModel) -> SidebarTools {
    let shown = app.selectedSpace.flatMap { $0.isIncognito ? nil : $0 }
    var tools = SidebarTools(store: app.store, shownSpace: shown)
    tools.otherWindowSpaces = Set(app.services.windows.filter { $0 !== app }.compactMap { $0.selectedSpace?.id })
    // Desktop-only pages are left out, as the phone's sidebar does.
    tools.isVisible = { $0.isPhoneVisible }
    tools.profileName = { [weak app] profile in app?.sync.profileName(account: profile) }
    return tools
  }

  private static func outcome(_ outcome: SidebarTools.Outcome) -> Result {
    outcome.isError ? .error(outcome.text) : .text(outcome.text)
  }

  /// The browser tools: the desktop's ClaudeTabs::RunTool(), and
  /// list_tabs, open_tab and select_tab.
  private func runBrowserTool(_ name: String, input: JSONValue) async -> Result {
    guard let app else { return .error("No window.") }
    let sidebar = sidebarTools(app)
    switch name {
    case "list_tabs": return listTabs(app, sidebar)
    case "open_tab": return await openTab(input)
    case "select_tab": return await selectTab(input)
    case "close_tab": return closeTab(input)
    case "activate_tab": return await activateTab(input)
    case "move_tab": return Self.outcome(sidebar.move(input))
    case "rename_tab": return Self.outcome(sidebar.rename(input))
    case "create_folder": return Self.outcome(sidebar.createFolder(input))
    case "split_tabs": return Self.outcome(sidebar.split(input))
    case "mute_tab": return muteTab(input)
    case "switch_space":
      guard let space = sidebar.space(input["space"]?.string) else { return .error("No such space. list_tabs lists them.") }
      guard app.visibleSpaces.contains(where: { $0 === space }) else { return .error("The window couldn't switch.") }
      app.selectedSpaceID = space.id
      return .text("The user's window shows \"\(space.name)\" now.")
    case "create_space":
      let (outcome, space) = sidebar.createSpace(input)
      guard var text = outcome.isError ? nil : outcome.text, let space else { return Self.outcome(outcome) }
      if input["show"] == true, app.visibleSpaces.contains(where: { $0 === space }) {
        app.selectedSpaceID = space.id
        text += " The user's window shows it."
      }
      return .text(text)
    case "search_history": return searchHistory(app, input)
    case "list_downloads": return listDownloads(app, input)
    case "send_tab_to_device": return sendTabToDevice(app, sidebar, input)
    default: return .error("Tool \(name) isn't available here.")
    }
  }

  /// The sidebar, as the desktop lists it (SidebarTools.listTabs), with
  /// what's live: titles and URLs of loaded pages, and the marks.
  private func listTabs(_ app: AppModel, _ sidebar: SidebarTools) -> Result {
    let shown = Set(app.services.windows.flatMap(\.displayedTabIDs))
    return .text(sidebar.listTabs { tab in
      let view = app.tabs.live[tab.id]
      return SidebarTools.TabState(
        title: view?.title, url: view?.url?.absoluteString, loaded: view != nil, front: shown.contains(tab.id),
        target: tab.id == targetTab, opened: opened.contains(tab.id), muted: app.tabs.muted.contains(tab.id),
        playing: app.tabs.playingVideo.contains(tab.id))
    })
  }

  private func openTab(_ input: JSONValue) async -> Result {
    guard let app, let spec = input["url"]?.string, let url = URL(string: spec),
      ["http", "https"].contains(url.scheme?.lowercased() ?? "")
    else { return .error("Claude can only open http and https web pages.") }
    // In the space of the tab Claude works on (else the shown one).
    let space = targetTab.flatMap { app.store.node(id: $0) }.flatMap { app.store.space(of: $0) } ?? app.selectedSpace
    guard let space, !space.isIncognito, let tab = app.store.addTab(url: url.absoluteString, to: space.today) else {
      return .error("The tab couldn't be opened.")
    }
    app.extensions.tabOpened(tab.id)
    opened.insert(tab.id)
    targetTab = tab.id
    if input["active"] == true { app.show(tab) }
    let view = app.tabs.webView(for: tab)
    Self.keepRendering(view)
    await Self.waitForLoad(view)
    return .text("Opened tab \(SidebarTools.nodeID(tab)): \(view.url?.absoluteString ?? url.absoluteString) (\"\(view.title ?? "")\"). The page tools act on it now; select_tab goes back.")
  }

  private func selectTab(_ input: JSONValue) async -> Result {
    let id = input["tab_id"]?.string ?? ""
    guard let app, let tab = sidebarTools(app).node(id) else { return .error("No tab \(id). list_tabs gives the ids.") }
    guard tab.isTab else { return .error("\(id) is a folder or a split: use one of its tabs.") }
    targetTab = tab.id
    // Not loaded: it loads now, in the background.
    let wasLoaded = app.tabs.live[tab.id] != nil
    let view = app.tabs.webView(for: tab)
    if !wasLoaded {
      Self.keepRendering(view)
      await Self.waitForLoad(view)
    }
    var text = "The page tools act on tab \(SidebarTools.nodeID(tab)) now: \(view.title ?? tab.displayTitle) — \(view.url?.absoluteString ?? tab.url)."
    if !["http", "https"].contains(view.url?.scheme?.lowercased() ?? "https") { text += " It's a browser page, which Claude can't use." }
    return .text(text)
  }

  /// activate_tab: shows the tab in the chat's window (its space too), and
  /// Claude acts on it.
  private func activateTab(_ input: JSONValue) async -> Result {
    let id = input["tab_id"]?.string ?? ""
    guard let app else { return .error("No window to show it in.") }
    guard var tab = sidebarTools(app).node(id) else { return .error("No tab \(id).") }
    if tab.isSplit, let first = tab.children.first { tab = first }
    guard tab.isTab else { return .error("That's a folder: activate one of its tabs.") }
    let wasLoaded = app.tabs.live[tab.id] != nil
    app.show(tab)
    targetTab = tab.id
    let view = app.tabs.webView(for: tab)
    if !wasLoaded { await Self.waitForLoad(view) }
    return .text("Showing \(id) (\"\(view.title ?? tab.displayTitle)\") to the user. The page tools act on it.")
  }

  /// Closes an entry as the desktop does, the iPhone's way: a tab Claude
  /// opened goes; a Today entry goes to the space's archive (as the user's
  /// close), or away with `remove`; a pinned entry or favorite unloads and
  /// stays, back on its pinned URL, unless `remove`. The system prompt has
  /// Claude ask before closing the user's tabs.
  private func closeTab(_ input: JSONValue) -> Result {
    let id = input["tab_id"]?.string ?? ""
    guard !id.isEmpty else { return .error("Give a tab_id from list_tabs.") }
    guard let app, let node = sidebarTools(app).node(id) else { return .error("No tab \(id).") }
    guard !node.isFolder else { return .error("That's a folder: close or move its tabs.") }
    let remove = input["remove"] == true
    let tabIDs = Set(node.tabs.map(\.id))
    for window in app.services.windows where window.selectedNode.map({ node.contains($0) || $0.contains(node) }) == true {
      window.selectedNodeID = nil
    }
    let favorite = node.section == .favorites
    var text: String
    if opened.contains(node.id) || remove || (node.section == .today && node.parent !== app.store.space(of: node)?.today) {
      opened.subtract(tabIDs)
      app.close(node)
      text = "Closed \(id)\(favorite ? " and removed it from Favorites" : "")."
    } else if node.parent === app.store.space(of: node)?.today {
      for tab in node.tabs { app.tabs.unload(tab.id) }
      app.store.archive(node)
      text = "Closed \(id); it's in the space's archive."
    } else {
      let live = node.tabs.filter { app.tabs.live[$0.id] != nil }
      for tab in node.tabs { app.unloadClosed(tab) }
      text = live.isEmpty
        ? "\(id) wasn't loaded; remove: true takes it out of the sidebar."
        : "Closed \(id); it stays in the sidebar, unloaded (remove: true takes it out)."
    }
    if let target = targetTab, tabIDs.contains(target) {
      targetTab = taskTab.flatMap { !tabIDs.contains($0) && app.store.node(id: $0) != nil ? $0 : nil }
      text += targetTab != nil ? " The page tools act on the chat's tab again." : " The page tools have no tab: select_tab one."
    }
    return .text(text)
  }

  /// mute_tab: a loaded tab's media now and after it loads again; an
  /// unloaded one when it loads (TabManager.muted, in memory).
  private func muteTab(_ input: JSONValue) -> Result {
    let id = input["tab_id"]?.string ?? ""
    guard let value = input["muted"], value == true || value == false else { return .error("Give tab_id and muted.") }
    guard let app, let tab = sidebarTools(app).node(id), tab.isTab else { return .error("No tab \(id).") }
    let muted = value == true
    if muted { app.tabs.muted.insert(tab.id) } else { app.tabs.muted.remove(tab.id) }
    if let view = app.tabs.live[tab.id] { Self.setMuted(view, muted) }
    return .text("\(muted ? "Muted" : "Unmuted") \(id).")
  }

  /// The script that keeps a tab's media muted, in every frame of its
  /// next pages.
  private static let muteUserScript = WKUserScript(
    source: SidebarTools.muteScript, injectionTime: .atDocumentStart, forMainFrameOnly: false, in: ClaudeChat.world)

  /// Mutes (or unmutes) a tab's media: the page now (its main frame), and
  /// its next pages through the user script. WebKit has no public way to
  /// mute a web view, so it's the page's audio and video elements (not Web
  /// Audio).
  static func setMuted(_ view: WKWebView, _ muted: Bool) {
    let controller = view.configuration.userContentController
    let has = controller.userScripts.contains { $0 === muteUserScript }
    if muted, !has { controller.addUserScript(muteUserScript) }
    if !muted, has {
      // One script can't be removed alone: the others go back.
      let others = controller.userScripts.filter { $0 !== muteUserScript }
      controller.removeAllUserScripts()
      others.forEach(controller.addUserScript)
    }
    view.evaluateJavaScript(
      muted ? SidebarTools.muteScript : SidebarTools.unmuteScript, in: nil, in: ClaudeChat.world, completionHandler: nil)
  }

  /// search_history: the History store (every profile's visits on this
  /// phone and those synced), one row per page, newest first.
  private func searchHistory(_ app: AppModel, _ input: JSONValue) -> Result {
    let limit = Int(min(max(input["max_results"]?.number ?? 20, 1), 100))
    let since = input["days"]?.number.flatMap { $0 >= 1 ? Date().addingTimeInterval(-$0 * 24 * 60 * 60) : nil }
    let rows = app.sync.history.search(input["query"]?.string ?? "", limit: limit, since: since)
    if rows.isEmpty { return .text("No matching history.") }
    return .text(rows.map { SidebarTools.historyLine(title: $0.title, url: $0.url, date: $0.lastVisit) }.joined())
  }

  /// list_downloads: Compass's downloads (running, and the files in
  /// Compass Browser › Downloads in the Files app), newest first.
  private func listDownloads(_ app: AppModel, _ input: JSONValue) -> Result {
    let limit = Int(min(max(input["limit"]?.number ?? 20, 1), 100))
    let items = app.downloads.items.sorted { $0.date > $1.date }.prefix(limit)
    if items.isEmpty { return .text("No downloads.") }
    return .text(items.map { item in
      let state = item.running ? "in progress" : item.failed == "Cancelled" ? "cancelled" : item.failed != nil ? "failed" : "done"
      var total: Int64?
      if item.file != nil { total = item.bytes }
      if item.running, let progress = item.progress, progress > 0 { total = Int64(Double(item.bytes) / progress) }
      let saved = item.file != nil
        ? "Compass Browser › Downloads › \(item.name) in the Files app"
        : "Compass Browser › Downloads in the Files app, once done"
      return SidebarTools.downloadLine(
        name: item.name, state: state, bytes: item.bytes, total: total, url: nil, savedAt: saved, date: item.date)
    }.joined())
  }

  /// send_tab_to_device: SEND_TAB_TO_SELF to another device of the tab's
  /// sync account (the desktop's ClaudeTabs::SendTabToDevice()).
  private func sendTabToDevice(_ app: AppModel, _ sidebar: SidebarTools, _ input: JSONValue) -> Result {
    let notSyncing = "Sending tabs needs Compass sync with another device."
    guard app.sync.isOn else { return .error(notSyncing) }
    // The tab: the one asked for, or the one Claude acts on.
    let tab = input["tab_id"]?.string.map { sidebar.node($0) } ?? targetTab.flatMap { app.store.node(id: $0) }
    let account = tab.map { app.sync.accountProfile(of: $0) }
      ?? app.selectedSpace.map { app.sync.accountProfile(of: $0) } ?? ""
    guard let devices = app.sync.sendTargets(account: account) else { return .error(notSyncing) }
    guard let name = input["device"]?.string, !devices.isEmpty else {
      if devices.isEmpty { return .text("No other devices sync with Compass.") }
      return .text("Devices:\n" + devices.map { "- \($0.name)\n" }.joined())
    }
    guard let device = devices.first(where: { $0.name.lowercased() == name.lowercased() }) else {
      return .error("No device \"\(name)\". Without device, the tool lists them.")
    }
    var url = "", title = ""
    if let tab, tab.isTab {
      let view = app.tabs.live[tab.id]
      url = view?.url?.absoluteString ?? tab.url
      title = view?.title.flatMap { $0.isEmpty ? nil : $0 } ?? tab.displayTitle
    }
    guard let parsed = URL(string: url), ["http", "https"].contains(parsed.scheme?.lowercased() ?? "") else {
      return .error("Only web pages can be sent; give a tab_id.")
    }
    app.sync.sendTab(url: url, title: title, to: device, account: account)
    return .text("Sent \(url) to \(device.name).")
  }

  /// Until the page has loaded (15 s at most), and a moment to settle.
  static func waitForLoad(_ view: WKWebView) async {
    let deadline = Date().addingTimeInterval(15)
    try? await Task.sleep(for: .milliseconds(300))
    while view.isLoading, Date() < deadline { try? await Task.sleep(for: .milliseconds(200)) }
    try? await Task.sleep(for: .milliseconds(300))
  }

  // MARK: Background tabs

  /// Holds web views Claude works on that no window shows: full size (so
  /// pages lay out as usual), behind everything, nearly transparent.
  private static var host: UIView?

  static func keepRendering(_ view: WKWebView) {
    guard view.window == nil || view.superview === host,
      let window = (UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        .flatMap(\.windows).first { $0.isKeyWindow })
    else { return }
    if host == nil || host?.window !== window {
      host?.removeFromSuperview()
      let view = UIView(frame: window.bounds)
      view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
      view.alpha = 0.01
      view.isUserInteractionEnabled = false
      window.insertSubview(view, at: 0)
      host = view
    }
    guard let host, view.superview !== host else { return }
    view.frame = host.bounds
    view.autoresizingMask = [.flexibleWidth, .flexibleHeight]
    host.addSubview(view)
  }

  /// Claude is done: the views it kept rendering go back to being unloaded
  /// from view (a window that shows one takes it).
  static func releaseRendering() {
    host?.subviews.forEach { $0.removeFromSuperview() }
  }

  // MARK: Click indicator

  /// After a click or hover: a dot where it landed, on the page (when it's
  /// on screen), fading out.
  private func acted(_ view: WKWebView, _ result: Result) async -> Result {
    guard !result.isError, view.window != nil, view.superview !== Self.host,
      let text = result.content.first?["text"]?.string, let object = JSONValue.parse(text),
      let x = object["x"]?.number, let y = object["y"]?.number
    else { return result }
    let cssWidth = (try? await view.evaluateJavaScript("window.visualViewport ? visualViewport.width : innerWidth", in: nil, contentWorld: ClaudeChat.world)) as? Double
    var visible = view.bounds
    if #available(iOS 26.0, *) { visible = visible.inset(by: view.obscuredContentInsets) }
    let points = Double(visible.width) / (cssWidth ?? Double(visible.width))
    let center = CGPoint(x: visible.minX + x * points, y: visible.minY + y * points)
    let dot = UIView(frame: CGRect(x: 0, y: 0, width: 18, height: 18))
    dot.center = center
    dot.layer.cornerRadius = 9
    dot.backgroundColor = UIColor.systemOrange.withAlphaComponent(0.85)
    dot.layer.borderColor = UIColor.white.cgColor
    dot.layer.borderWidth = 2
    dot.isUserInteractionEnabled = false
    view.addSubview(dot)
    UIView.animate(withDuration: 0.25, animations: { dot.transform = CGAffineTransform(scaleX: 1.6, y: 1.6) }) { _ in
      UIView.animate(withDuration: 0.6, delay: 0.4, animations: { dot.alpha = 0 }) { _ in dot.removeFromSuperview() }
    }
    return result
  }

  // MARK: Page agent

  /// `coordinate` in screenshot pixels becomes CSS pixels, as the page
  /// agent wants.
  private func cssInput(_ input: JSONValue) -> JSONValue {
    guard var object = input.object, let point = object["coordinate"]?.array, point.count == 2,
      let x = point[0].number, let y = point[1].number
    else { return input }
    object["coordinate"] = [.number(x / screenshotScale), .number(y / screenshotScale)]
    return .object(object)
  }

  func agent(_ view: WKWebView, _ method: String, _ input: JSONValue) async -> Result {
    let body = ClaudeSettings.pageAgent + "\n;return JSON.stringify(globalThis.__compassAgent.\(method)(JSON.parse(opts)));"
    let options = String(decoding: input.encoded(), as: UTF8.self)
    do {
      let value = try await view.callAsyncJavaScript(body, arguments: ["opts": options], in: nil, contentWorld: ClaudeChat.world)
      guard let text = value as? String, var result = JSONValue.parse(text)?.object else {
        return .error("The page didn't answer (it may still be loading).")
      }
      guard result["ok"] == true else { return .error(result["error"]?.string ?? "The page couldn't be read.") }
      result["ok"] = nil
      // A tree or the page's text comes back as the desktop words it.
      if let tree = result["tree"]?.string {
        var text = Self.pageHeader(result)
        if let focused = result["focused"]?.string { text += "\nFocused: \(focused)" }
        return .text(text + "\n\n" + tree)
      }
      if method == "pageText", let body = result["text"]?.string {
        return .text(Self.pageHeader(result) + "\n\n" + body)
      }
      return .text(String(decoding: JSONValue.object(result).encoded(), as: UTF8.self))
    } catch {
      return .error("The page didn't answer: \(error.localizedDescription)")
    }
  }

  /// The page agent's title, URL and viewport, as the desktop's PageHeader.
  static func pageHeader(_ page: [String: JSONValue]) -> String {
    var header = "Title: \(page["title"]?.string ?? "")\nURL: \(page["url"]?.string ?? "")"
    if let v = page["viewport"] {
      func n(_ key: String) -> Int { Int(v[key]?.number ?? 0) }
      header += "\nViewport: \(n("width"))x\(n("height")), scrolled to \(n("scrollX")),\(n("scrollY")) of \(n("scrollWidth"))x\(n("scrollHeight"))"
    }
    return header
  }

  // MARK: Screenshot

  private func screenshot(_ view: WKWebView, input: JSONValue) async -> Result {
    if let ref = input["ref"]?.string {
      let scrolled = await agent(view, "scrollIntoView", ["ref": .string(ref)])
      if scrolled.isError { return scrolled }
      try? await Task.sleep(for: .milliseconds(300))
    }
    // What the user sees of the page: not what's under the status bar or
    // the bottom bar.
    var visible = view.bounds
    if #available(iOS 26.0, *) {
      visible = visible.inset(by: view.obscuredContentInsets)
    }
    let configuration = WKSnapshotConfiguration()
    configuration.rect = visible
    let longest = max(visible.width, visible.height) * view.traitCollection.displayScale
    let outScale = min(1, Self.screenshotMaxEdge / longest)
    // In points; the image has the screen's scale.
    configuration.snapshotWidth = NSNumber(value: Double(visible.width * outScale))
    guard let image = try? await view.takeSnapshot(configuration: configuration),
      let jpeg = image.jpegData(compressionQuality: 0.8)
    else { return .error("No screenshot.") }
    let width = Int(image.size.width * image.scale)
    let height = Int(image.size.height * image.scale)
    // CSS pixels across the visible page (zoom included).
    let cssWidth = (try? await view.evaluateJavaScript("window.visualViewport ? visualViewport.width : innerWidth", in: nil, contentWorld: ClaudeChat.world)) as? Double
    screenshotScale = Double(width) / (cssWidth ?? Double(visible.width))
    return Result(content: [
      ["type": "image", "source": ["type": "base64", "media_type": "image/jpeg", "data": .string(jpeg.base64EncodedString())]],
      ["type": "text", "text": .string("Screenshot of the visible page, \(width)x\(height). Use this image's pixels for coordinates.")],
    ])
  }

  // MARK: Console and JavaScript

  private func readConsole(_ view: WKWebView, input: JSONValue) async -> Result {
    let onlyNew = input["only_new"].map { $0 == true } ?? true
    let limit = Int(min(max(input["limit"]?.number ?? 100, 1), 500))
    let levels = input["levels"]?.array?.compactMap(\.string) ?? []
    let pattern = input["pattern"]?.string.flatMap { try? NSRegularExpression(pattern: $0) }
    if input["pattern"]?.string != nil, pattern == nil { return .error("Bad pattern.") }
    let value = try? await view.evaluateJavaScript(ConsoleCapture.readScript, in: nil, contentWorld: .page)
    guard let text = value as? String, let object = JSONValue.parse(text) else {
      return .text("No console messages (the page may have loaded before Claude could listen).")
    }
    let total = Int(object["total"]?.number ?? 0)
    var entries = object["entries"]?.array ?? []
    let first = total - entries.count
    if onlyNew { entries = Array(entries.dropFirst(max(0, consoleRead - first))) }
    consoleRead = total
    entries = entries.filter { entry in
      let level = entry["level"]?.string ?? "log"
      let text = entry["text"]?.string ?? ""
      return (levels.isEmpty || levels.contains(level))
        && (pattern?.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil || pattern == nil)
    }
    if entries.isEmpty { return .text(onlyNew ? "No new console messages." : "No console messages.") }
    var out = ""
    let skipped = max(0, entries.count - limit)
    if skipped > 0 { out += "\(skipped) earlier messages left out.\n" }
    for entry in entries.suffix(limit) {
      out += "[\(entry["level"]?.string ?? "log")] \(entry["text"]?.string ?? "")"
      if let source = entry["source"]?.string, !source.isEmpty { out += "  (\(source))" }
      out += "\n"
    }
    return .text(out)
  }

  private func runJS(_ view: WKWebView, input: JSONValue) async -> Result {
    guard let code = input["code"]?.string, !code.isEmpty else { return .error("No code.") }
    do {
      let value = try await view.evaluateJavaScript(code, in: nil, contentWorld: .page)
      var text: String
      if let value, JSONSerialization.isValidJSONObject(value),
        let data = try? JSONSerialization.data(withJSONObject: value, options: [.sortedKeys])
      {
        text = String(decoding: data, as: UTF8.self)
      } else if let value {
        text = value is String ? "\"\(value)\"" : "\(value)"
      } else {
        text = "undefined"
      }
      if text.count > Self.maxJSChars { text = String(text.prefix(Self.maxJSChars)) + "\n… (cut)" }
      return .text(text)
    } catch {
      let message = (error as NSError).userInfo["WKJavaScriptExceptionMessage"] as? String ?? error.localizedDescription
      return .error("Uncaught: \(message)")
    }
  }

  // MARK: Navigation

  private func navigate(_ node: SidebarNode, input: JSONValue) async -> Result {
    guard let app, let where_ = input["url"]?.string?.trimmingCharacters(in: .whitespaces), !where_.isEmpty else {
      return .error("No URL.")
    }
    let view = app.tabs.webView(for: node)
    switch where_ {
    case "back":
      guard view.canGoBack else { return .error("There's no page to go back to.") }
      view.goBack()
    case "forward":
      guard view.canGoForward else { return .error("There's no page to go forward to.") }
      view.goForward()
    case "reload":
      view.reload()
    default:
      guard let url = URL(string: where_), ["http", "https"].contains(url.scheme?.lowercased() ?? "") else {
        return .error("Only http(s) URLs.")
      }
      view.load(URLRequest(url: url))
    }
    Self.keepRendering(view)
    await Self.waitForLoad(view)
    reset()
    return .text("Now at \(view.url?.absoluteString ?? where_)\(view.title.map { " (\($0))" } ?? "").")
  }
}

/// The page's console, kept for read_console: a document-start script in
/// the page world (main frame) wraps console.*, `error` and
/// `unhandledrejection`, and keeps the last 500 messages.
@MainActor
enum ConsoleCapture {
  static let script = WKUserScript(source: source, injectionTime: .atDocumentStart, forMainFrameOnly: true, in: .page)

  private static let source = """
    (() => {
      if (window.__compassConsole) return;
      const entries = [];
      let total = 0;
      const text = (args) => args.map(a => {
        if (typeof a === 'string') return a;
        if (a instanceof Error) return a.stack || String(a);
        try { return JSON.stringify(a); } catch { return String(a); }
      }).join(' ').slice(0, 2000);
      const add = (level, message, source) => {
        entries.push({level, text: message, source: source || ''});
        if (entries.length > 500) entries.shift();
        total++;
      };
      for (const [method, level] of [['debug', 'debug'], ['log', 'log'], ['info', 'info'], ['warn', 'warning'], ['error', 'error']]) {
        const original = console[method];
        console[method] = function (...args) {
          try { add(level, text(args)); } catch {}
          return original.apply(this, args);
        };
      }
      addEventListener('error', e => add('error', 'Uncaught ' + (e.error && e.error.stack || e.message), e.filename ? e.filename + ':' + e.lineno : ''));
      addEventListener('unhandledrejection', e => add('error', 'Unhandled rejection: ' + text([e.reason])));
      Object.defineProperty(window, '__compassConsole', {
        value: () => JSON.stringify({total, entries}), enumerable: false,
      });
    })();
    """

  static let readScript = "window.__compassConsole ? window.__compassConsole() : null"
}
