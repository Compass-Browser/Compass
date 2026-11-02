// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import Foundation
import Observation
import WebKit

/// One window's Claude conversation (the desktop keeps one per window too),
/// in memory, kept when the sheet closes. I2: text only, with the shown
/// page's text attached to a message; the page tools come with I3. The
/// loop is the desktop's (claude_chat/app.ts).
@MainActor
@Observable
final class ClaudeChat {
  private(set) var conversation = Conversation()
  private(set) var streaming = false
  /// The next message carries the shown page's text (on by default; the
  /// same page isn't sent twice in a row).
  var attachPage = true
  @ObservationIgnored private var attachedURL = ""
  @ObservationIgnored private var task: Task<Void, Never>?
  @ObservationIgnored private var revision = 0

  /// The page's text, at most this long (the desktop's).
  static let maxPageChars = 50000
  /// Claude's own content world for the page script.
  static let world = WKContentWorld.world(name: "claude")

  var isEmpty: Bool { conversation.turns.isEmpty }

  func send(_ text: String, app: AppModel) {
    let text = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !text.isEmpty, !streaming else { return }
    streaming = true
    conversation.closeOpenTools("Not run: the reply ended first.")
    let view = app.shownTab.flatMap { app.tabs.live[$0.id] }
    let wantsPage = attachPage && view?.url?.absoluteString != attachedURL
    task = Task { [weak self] in
      var page: (url: String, title: String, text: String, truncated: Bool)?
      if wantsPage, let view { page = await Self.pageText(view) }
      guard let self, !Task.isCancelled else { return }
      if let page { self.attachedURL = page.url }
      self.conversation.addUser(text: text, page: page)
      await self.reply()
    }
  }

  /// The last message again, after a failed reply.
  func retry() {
    guard !streaming, let last = conversation.turns.last, last.role == .assistant, last.error != nil else { return }
    streaming = true
    task = Task { [weak self] in await self?.reply() }
  }

  func stop() {
    task?.cancel()
    task = nil
    conversation.finishReply()
    conversation.closeOpenTools("Stopped by the user.")
    streaming = false
  }

  func newChat() {
    stop()
    conversation.clear()
    attachedURL = ""
    attachPage = true
  }

  private func reply() async {
    let settings = ClaudeSettings.shared
    guard let endpoint = settings.endpoint else {
      conversation.startReply()
      conversation.finishReply(error: "Add an API key in Settings → Claude.")
      streaming = false
      return
    }
    let started = settings.revision
    let options = ChatRequest.Options(
      model: settings.model, systemPrompt: ClaudeSettings.systemPrompt, browserScope: settings.browserScope,
      claudeAPI: endpoint.isClaudeAPI)
    let request = ChatRequest(messages: conversation.messages(), options: options)
    conversation.startReply()
    let error = await ClaudeClient.stream(request, endpoint: endpoint) { [weak self] name, data in
      guard let self, self.streaming, settings.revision == started else { return }
      self.conversation.apply(event: name, data: data)
    }
    guard streaming else { return }  // stopped
    conversation.finishReply(error: settings.revision == started ? error : "The key or endpoint changed.")
    streaming = false
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
