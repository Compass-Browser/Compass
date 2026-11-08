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
  }

  private(set) var conversation = Conversation()
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
    task?.cancel()
    task = nil
    conversation.finishReply()
    closeOpenTools("Stopped by the user.")
    streaming = false
    actingTab = nil
    ClaudeTools.releaseRendering()
  }

  func newChat() {
    stop()
    conversation.clear()
    toolViews = [:]
    tools?.forget()
    attachedURL = ""
    attachPage = true
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
    let options = ChatRequest.Options(
      model: settings.model, systemPrompt: ClaudeSettings.systemPrompt, tools: ClaudeSettings.tools,
      browserScope: settings.browserScope, windowTabs: windowTabs, claudeAPI: endpoint.isClaudeAPI,
      oauthToken: endpoint.usesOAuthToken)
    let request = ChatRequest(messages: conversation.messages(), options: options)
    conversation.startReply()
    let error = await ClaudeClient.stream(request, endpoint: endpoint) { [weak self] name, data in
      guard let self, self.streaming, settings.revision == started else { return }
      self.conversation.apply(event: name, data: data)
    }
    guard streaming, !Task.isCancelled else { return }  // stopped
    conversation.finishReply(error: settings.revision == started ? error : "The key or endpoint changed.")
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
      var view = ToolView(running: false, isError: result.isError)
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
