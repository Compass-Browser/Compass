// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import Foundation
import Observation
import UIKit
import WebKit

/// One window's Claude conversation (the desktop keeps one per window too),
/// in memory, kept when the sheet closes. A message carries the shown
/// page's text when attached; Claude's tool calls run on the tab the chat
/// is about (ClaudeTools), one after the other, and their results go back
/// until Claude ends its turn. The loop is the desktop's (claude_chat/app.ts).
@MainActor
@Observable
final class ClaudeChat {
  /// A tool call's state, as its row in the reply shows it.
  struct ToolView {
    var running = true
    var isError = false
    var text = ""
    var image: UIImage?
    /// What a memory tool changed: shown as a line with Undo.
    var memory: ClaudeTools.MemoryChange?
    var memoryUndone = false
  }

  /// A write waiting for the user (the approval card).
  struct Approval: Identifiable {
    let id = UUID()
    let tool: String
    let title: String
    let detail: String
  }

  enum Decision {
    case allow
    /// That tool, until New Chat.
    case allowForChat
    case deny
  }

  private(set) var conversation = Conversation()
  /// Writes waiting for the user, in order; the card shows the first.
  private(set) var approvals: [Approval] = []
  @ObservationIgnored private var waiting: [UUID: CheckedContinuation<Bool, Never>] = [:]
  /// Tools allowed for this chat ("Allow for This Chat").
  @ObservationIgnored private var allowedForChat: Set<String> = []
  private(set) var streaming = false
  /// Each tool call's state, by tool_use id.
  private(set) var toolViews: [String: ToolView] = [:]
  /// The tab Claude is acting on while it runs tools (its sidebar row is
  /// tinted).
  private(set) var actingTab: String?
  /// The next message carries the shown page's text (on by default; the
  /// same page isn't sent twice in a row).
  var attachPage = true
  @ObservationIgnored private var attachedURL = ""
  @ObservationIgnored private var task: Task<Void, Never>?
  @ObservationIgnored private var tools: ClaudeTools?
  /// The memory this chat started with, and whether it may save more: in
  /// the system prompt, which never changes within a chat.
  @ObservationIgnored private var chatMemory: (block: String, tools: Bool)?
  /// The developer tools were on at some message of this chat: their
  /// definitions stay for the rest of it (a conversation's tools only grow,
  /// as on the desktop); while off they refuse.
  @ObservationIgnored private var chatDevTools = false

  /// The page's text, at most this long (the desktop's).
  static let maxPageChars = 50000
  /// Tool rounds per message before Claude is stopped (the desktop's).
  static let maxToolRounds = 40
  /// Claude's own content world for the page script.
  static let world = WKContentWorld.world(name: "claude")

  var isEmpty: Bool { conversation.turns.isEmpty }

  func send(_ text: String, app: AppModel) {
    let text = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !text.isEmpty, !streaming, app.shownTab.map({ app.tabs.isIncognito($0.id) }) != true else { return }
    streaming = true
    conversation.closeOpenTools("Not run: the reply ended first.")
    let tools = self.tools ?? ClaudeTools(app: app)
    self.tools = tools
    tools.approve = { [weak self] approval in await self?.ask(approval) ?? false }
    // Claude's tools act on this tab until the next message.
    tools.targetTab = app.shownTab?.id
    tools.taskTab = app.shownTab?.id
    let view = app.shownTab.flatMap { app.tabs.live[$0.id] }
    let wantsPage = attachPage && view?.url?.absoluteString != attachedURL
    let windowTabs = Self.windowTabs(app)
    task = Task { [weak self] in
      var page: (url: String, title: String, text: String, truncated: Bool)?
      if wantsPage, let view { page = await Self.pageText(view) }
      guard let self, !Task.isCancelled else { return }
      if let page { self.attachedURL = page.url }
      self.conversation.addUser(text: text, page: page)
      await self.run(windowTabs: windowTabs)
    }
  }

  /// The last message again, after a failed reply.
  func retry(app: AppModel) {
    guard !streaming, let last = conversation.turns.last, last.role == .assistant, last.error != nil else { return }
    streaming = true
    let windowTabs = Self.windowTabs(app)
    task = Task { [weak self] in await self?.run(windowTabs: windowTabs) }
  }

  func stop() {
    denyAll()
    task?.cancel()
    task = nil
    conversation.finishReply()
    closeOpenTools("Stopped by the user.")
    streaming = false
    actingTab = nil
    ClaudeTools.releaseRendering()
    tools?.endEmulation()
  }

  func newChat() {
    stop()
    conversation.clear()
    toolViews = [:]
    tools?.forget()
    attachedURL = ""
    attachPage = true
    chatMemory = nil
    chatDevTools = false
    allowedForChat = []
  }

  /// Asks the user before a file is written; true to go ahead.
  func ask(_ request: ClaudeTools.Approval) async -> Bool {
    if allowedForChat.contains(request.tool) { return true }
    guard streaming else { return false }
    let approval = Approval(tool: request.tool, title: request.title, detail: request.detail)
    return await withCheckedContinuation { continuation in
      waiting[approval.id] = continuation
      approvals.append(approval)
    }
  }

  /// The card's answer.
  func answer(_ id: UUID, _ decision: Decision) {
    guard let continuation = waiting.removeValue(forKey: id) else { return }
    if decision == .allowForChat, let approval = approvals.first(where: { $0.id == id }) {
      allowedForChat.insert(approval.tool)
    }
    approvals.removeAll { $0.id == id }
    continuation.resume(returning: decision != .deny)
  }

  /// Stop and New Chat: whatever waits is denied.
  private func denyAll() {
    let pending = waiting
    waiting = [:]
    approvals = []
    for continuation in pending.values { continuation.resume(returning: false) }
  }

  /// A memory line's Undo: the memory goes back to what it was.
  func undoMemory(_ toolID: String) {
    guard var view = toolViews[toolID], let change = view.memory, !view.memoryUndone else { return }
    let memory = ClaudeMemoryStore.shared
    let undone = change.before.map { memory.restore($0) } ?? (memory.remove(change.id) != nil)
    if undone {
      view.memoryUndone = true
      toolViews[toolID] = view
    }
  }

  /// The chat's memory, taken when its first message goes.
  private func frozenMemory() -> (block: String, tools: Bool) {
    if let chatMemory { return chatMemory }
    let settings = ClaudeSettings.shared
    let memory: (block: String, tools: Bool) =
      settings.memoryEnabled ? (ClaudeMemoryStore.shared.promptBlock, settings.memorySave) : ("", false)
    chatMemory = memory
    return memory
  }

  /// Replies, and tool rounds while Claude asks for tools.
  private func run(windowTabs: String) async {
    var rounds = 0
    while streaming, !Task.isCancelled {
      await reply(windowTabs: windowTabs)
      guard streaming, !Task.isCancelled, let turn = conversation.turns.last, turn.role == .assistant,
        turn.error == nil, turn.stopReason == "tool_use"
      else { break }
      rounds += 1
      if rounds > Self.maxToolRounds {
        closeOpenTools("Not run: Claude used many steps; the user may ask to go on.")
        conversation.failLastReply("Claude stopped after \(Self.maxToolRounds) steps.")
        break
      }
      await runTools(turn)
    }
    if !Task.isCancelled { streaming = false }
    actingTab = nil
    ClaudeTools.releaseRendering()
  }

  private func reply(windowTabs: String) async {
    let settings = ClaudeSettings.shared
    guard let endpoint = settings.endpoint else {
      conversation.startReply()
      conversation.finishReply(error: "Add an API key in Settings → Claude.")
      return
    }
    if endpoint.needsTokenRisksAccepted {
      conversation.startReply()
      conversation.finishReply(error: ClaudeEndpoint.tokenRisksMessage)
      return
    }
    let started = settings.revision
    let memory = frozenMemory()
    if settings.devtools { chatDevTools = true }
    var options = ChatRequest.Options(
      model: settings.model, systemPrompt: ClaudeSettings.systemPrompt,
      tools: ClaudeSettings.tools + (memory.tools ? ClaudeSettings.memoryTools : [])
        + (chatDevTools ? ClaudeSettings.devtoolsTools : []),
      browserScope: settings.browserScope, windowTabs: windowTabs, claudeAPI: endpoint.isClaudeAPI,
      oauthToken: endpoint.usesOAuthToken, memory: memory.block, files: settings.filesContextLine,
      devtools: settings.devtools)
    options.minimalCaching = Self.minimalCaching
    conversation.startReply()
    var error = await stream(ChatRequest(messages: conversation.messages(), options: options), endpoint, started)
    // An endpoint that adds cache breakpoints of its own refuses ours past
    // the API's 4: ask again with only the top-level one, and from now on.
    if !options.minimalCaching, ChatRequest.isTooManyCacheBreakpoints(error), streaming, !Task.isCancelled,
      conversation.streamingTurn?.content.isEmpty ?? false
    {
      Self.minimalCaching = true
      options.minimalCaching = true
      error = await stream(ChatRequest(messages: conversation.messages(), options: options), endpoint, started)
    }
    guard streaming, !Task.isCancelled else { return }  // stopped
    conversation.finishReply(error: settings.revision == started ? error : "The key or endpoint changed.")
  }

  /// Set once an endpoint refused the usual cache breakpoints, for the rest
  /// of the app's run (`ChatRequest.Options.minimalCaching`).
  private static var minimalCaching = false

  private func stream(_ request: ChatRequest, _ endpoint: ClaudeEndpoint, _ started: Int) async -> String? {
    let settings = ClaudeSettings.shared
    return await ClaudeClient.stream(request, endpoint: endpoint) { [weak self] name, data in
      guard let self, self.streaming, settings.revision == started else { return }
      self.conversation.apply(event: name, data: data)
    }
  }

  /// Runs the reply's tool calls one after the other, then adds their
  /// results.
  private func runTools(_ turn: Conversation.Turn) async {
    guard let tools else { return }
    let calls = turn.content.filter { $0["type"]?.string == "tool_use" }
    var results: [JSONValue] = []
    for call in calls {
      guard let id = call["id"]?.string else { continue }
      if Task.isCancelled || !streaming {
        results.append(Self.result(id, .error("Stopped by the user.")))
        continue
      }
      toolViews[id] = ToolView()
      actingTab = tools.targetTab
      let result = await tools.run(call["name"]?.string ?? "", input: call["input"] ?? .object([:]))
      var view = ToolView(running: false, isError: result.isError, memory: result.memoryChange)
      for block in result.content {
        if let text = block["text"]?.string { view.text += text }
        if let data = block["source"]?["data"]?.string, let bytes = Data(base64Encoded: data) {
          view.image = UIImage(data: bytes)
        }
      }
      toolViews[id] = view
      results.append(Self.result(id, result))
    }
    conversation.addToolResults(results)
    actingTab = tools.targetTab
  }

  private static func result(_ id: String, _ result: ClaudeTools.Result) -> JSONValue {
    var block: [String: JSONValue] = ["type": "tool_result", "tool_use_id": .string(id), "content": .array(result.content)]
    if result.isError { block["is_error"] = true }
    return .object(block)
  }

  private func closeOpenTools(_ why: String) {
    if let last = conversation.turns.last, last.role == .assistant {
      for block in last.content where block["type"]?.string == "tool_use" {
        if let id = block["id"]?.string, toolViews[id]?.running ?? true {
          toolViews[id] = ToolView(running: false, isError: true, text: why)
        }
      }
    }
    conversation.closeOpenTools(why)
  }

  /// The shown space's open tabs, titles and URLs, as the desktop sends its
  /// window's (data, not instructions; the prompt says so).
  private static func windowTabs(_ app: AppModel) -> String {
    guard let space = app.selectedSpace else { return "" }
    let tabs = (space.pinned.children.flatMap(\.subtree) + space.today.children.reversed().flatMap(\.subtree))
      .filter { $0.isTab && $0.isPhoneVisible }
      .prefix(40)
    return tabs.map { "- \($0.displayTitle) — \($0.url)" }.joined(separator: "\n")
  }

  /// The page's main text, through the shared page script.
  private static func pageText(_ view: WKWebView) async -> (url: String, title: String, text: String, truncated: Bool)? {
    guard let url = view.url, ["http", "https"].contains(url.scheme?.lowercased() ?? "") else { return nil }
    let body = ClaudeSettings.pageAgent + "\n;return globalThis.__compassAgent.pageText(opts);"
    guard let result = try? await view.callAsyncJavaScript(
      body, arguments: ["opts": ["max_chars": maxPageChars]], in: nil, contentWorld: world) as? [String: Any],
      result["ok"] as? Bool == true
    else { return nil }
    return (
      result["url"] as? String ?? url.absoluteString, result["title"] as? String ?? view.title ?? "",
      result["text"] as? String ?? "", result["truncated"] as? Bool ?? false
    )
  }
}
