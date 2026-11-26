// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import CompassClaude
import SwiftUI

/// The Claude chat sheet (docs/design/claude.md, "iOS"): at the medium
/// detent the page stays visible above it. The window's conversation, a
/// field to write in, Stop while Claude answers, New Chat, and the page
/// attachment.
struct ClaudeChatSheet: View {
  @Environment(AppModel.self) private var app
  @State private var showingSettings = false
  @FocusState private var focused: Bool

  private var chat: ClaudeChat { app.claude }
  private var settings: ClaudeSettings { .shared }

  var body: some View {
    NavigationStack {
      Group {
        if app.shownTab.map({ app.tabs.isIncognito($0.id) }) == true {
          ContentUnavailableView(
            "Not in Incognito", systemImage: "eyeglasses",
            description: Text("Claude doesn't read or act on Incognito tabs. Open a normal tab to ask about it."))
        } else if !settings.hasKey {
          noKey
        } else if chat.isEmpty {
          emptyState
        } else {
          messages
        }
      }
      .safeAreaInset(edge: .bottom) {
        if settings.hasKey, app.shownTab.map({ app.tabs.isIncognito($0.id) }) != true { composer }
      }
      .navigationTitle(ClaudeSettings.shared.provider.assistantName)
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .topBarLeading) {
          Button("New Chat", systemImage: "square.and.pencil") { chat.newChat() }
            .disabled(chat.isEmpty)
        }
        ToolbarItem(placement: .topBarTrailing) {
          Menu {
            Picker("Model", selection: Binding(get: { settings.model }, set: { settings.model = $0 })) {
              ForEach(settings.pickerModels) { Text($0.displayName).tag($0.id) }
            }
            Button("Claude Settings", systemImage: "gear") { showingSettings = true }
          } label: {
            Image(systemName: "ellipsis.circle")
          }
        }
      }
      .sheet(isPresented: $showingSettings) {
        NavigationStack {
          ClaudeSettingsView().toolbar { Button("Done") { showingSettings = false } }
        }
      }
    }
    .presentationDetents([.medium, .large])
    .presentationDragIndicator(.visible)
  }

  private var noKey: some View {
    ContentUnavailableView {
      Label("Add an API Key", systemImage: "key")
    } description: {
      Text("Claude needs an Anthropic API key, or a compatible endpoint. Compass doesn't come with one.")
    } actions: {
      Button("Open Claude Settings") { showingSettings = true }.buttonStyle(.borderedProminent)
    }
  }

  private var emptyState: some View {
    ContentUnavailableView {
      Label("Ask about this page", systemImage: "sparkles")
    } description: {
      Text("Claude gets the page's text with your message. Page content is treated as data, never as instructions.")
    }
  }

  private var messages: some View {
    ScrollViewReader { proxy in
      ScrollView {
        LazyVStack(alignment: .leading, spacing: 14) {
          ForEach(chat.conversation.turns.filter { !$0.isToolResults }) { turn in
            TurnView(
              turn: turn, tools: chat.toolViews, undoMemory: { chat.undoMemory($0) }, undoBoost: { chat.undoBoost($0) }
            ) {
              chat.retry(app: app)
            }.id(turn.id)
          }
          Color.clear.frame(height: 1).id("end")
        }
        .padding()
      }
      .scrollDismissesKeyboard(.interactively)
      // Links in replies open in a new tab, under the sheet (the desktop's
      // rule); mail goes to the system.
      .environment(
        \.openURL,
        OpenURLAction { url in
          guard url.scheme == "http" || url.scheme == "https" else { return .systemAction }
          app.openLink(url)
          return .handled
        })
      .onChange(of: chat.conversation.turns.last?.content.count) { proxy.scrollTo("end", anchor: .bottom) }
      .onChange(of: chat.conversation.turns.count) { withAnimation { proxy.scrollTo("end", anchor: .bottom) } }
    }
  }

  private var composer: some View {
    VStack(spacing: 6) {
      if let approval = chat.approvals.first {
        ApprovalCard(approval: approval, more: chat.approvals.count - 1) { chat.answer(approval.id, $0) }
      }
      if let tab = app.shownTab {
        Button {
          chat.attachPage.toggle()
        } label: {
          HStack(spacing: 6) {
            SiteIcon(url: tab.url, size: 14)
            Text(tab.displayTitle).lineLimit(1)
            Image(systemName: chat.attachPage ? "checkmark.circle.fill" : "circle")
          }
          .font(.caption)
          .padding(.horizontal, 10)
          .padding(.vertical, 5)
          .background(.quaternary, in: Capsule())
          .opacity(chat.attachPage ? 1 : 0.6)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(chat.attachPage ? LocalizedStringKey("Page attached") : "Page not attached")
        .frame(maxWidth: .infinity, alignment: .leading)
      }
      if !chat.queued.isEmpty {
        QueuedList(chat: chat)
      }
      HStack(alignment: .bottom, spacing: 8) {
        TextField(ClaudeSettings.shared.provider.askTitle, text: Bindable(chat).draft, axis: .vertical)
          .lineLimit(1...6)
          .focused($focused)
          .submitLabel(.send)
          .onSubmit(send)
          .padding(.horizontal, 12)
          .padding(.vertical, 8)
          .background(.quaternary, in: RoundedRectangle(cornerRadius: 18))
          .accessibilityIdentifier("ClaudeInput")
        if chat.streaming {
          if !draftIsEmpty {
            Button(action: send) { Image(systemName: "arrow.up.circle").font(.title) }
              .accessibilityLabel("Send After This Reply")
              .accessibilityIdentifier("ClaudeQueue")
          }
          Button { chat.stop() } label: { Image(systemName: "stop.circle.fill").font(.title) }
            .accessibilityLabel("Stop")
        } else {
          Button(action: send) { Image(systemName: "arrow.up.circle.fill").font(.title) }
            .disabled(draftIsEmpty && chat.queued.isEmpty)
            .accessibilityLabel("Send")
        }
      }
    }
    .padding(.horizontal)
    .padding(.vertical, 8)
    .background(.bar)
  }

  private var draftIsEmpty: Bool { chat.draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }

  private func send() {
    chat.submit(app: app)
  }
}

/// Messages written while Claude replies, sent one by one after it: a tap
/// puts one back in the field, × removes it.
private struct QueuedList: View {
  let chat: ClaudeChat

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      Text(chat.streaming ? "Sends after this reply" : "Paused: send to go on")
        .font(.caption)
        .foregroundStyle(.secondary)
      ForEach(Array(chat.queued.enumerated()), id: \.offset) { index, text in
        HStack(spacing: 4) {
          Button { chat.editQueued(at: index) } label: {
            Text(text).lineLimit(1).frame(maxWidth: .infinity, alignment: .leading)
          }
          .buttonStyle(.plain)
          .accessibilityHint("Edit")
          Button { chat.removeQueued(at: index) } label: { Image(systemName: "xmark.circle.fill") }
            .buttonStyle(.plain)
            .foregroundStyle(.secondary)
            .accessibilityLabel("Remove")
        }
        .font(.subheadline)
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(.quaternary, in: Capsule())
      }
    }
    .frame(maxWidth: .infinity, alignment: .leading)
  }
}

/// A file write waiting for the user (docs/design/claude.md, "Files"):
/// what and where, the content's start, and Allow / Allow for This Chat /
/// Deny.
private struct ApprovalCard: View {
  let approval: ClaudeChat.Approval
  let more: Int
  let answer: (ClaudeChat.Decision) -> Void

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Label(approval.title, systemImage: approval.symbol)
        .font(.subheadline.weight(.semibold))
      if !approval.detail.isEmpty {
        ScrollView {
          Text(approval.detail)
            .font(.caption.monospaced())
            .frame(maxWidth: .infinity, alignment: .leading)
            .textSelection(.enabled)
        }
        .frame(maxHeight: 120)
        .padding(8)
        .background(.quaternary, in: RoundedRectangle(cornerRadius: 8))
      }
      HStack {
        Button("Deny", role: .cancel) { answer(.deny) }
          .accessibilityIdentifier("ClaudeApprovalDeny")
        Spacer()
        if approval.allowForChat {
          Button("Allow for This Chat") { answer(.allowForChat) }
            .accessibilityIdentifier("ClaudeApprovalAllowChat")
        }
        Button("Allow") { answer(.allow) }
          .buttonStyle(.borderedProminent)
          .accessibilityIdentifier("ClaudeApprovalAllow")
      }
      .font(.subheadline)
      if more > 0 {
        Text("\(more) more waiting").font(.caption).foregroundStyle(.secondary)
      }
    }
    .padding(12)
    .background(.background, in: RoundedRectangle(cornerRadius: 14))
    .overlay(RoundedRectangle(cornerRadius: 14).stroke(.orange.opacity(0.6)))
  }
}

/// One turn: the user's message (with the page it carried), or Claude's
/// reply: its thinking summary, its text, and how it ended.
private struct TurnView: View {
  let turn: Conversation.Turn
  let tools: [String: ClaudeChat.ToolView]
  let undoMemory: (String) -> Void
  let undoBoost: (String) -> Void
  let retry: () -> Void

  var body: some View {
    if turn.role == .user {
      VStack(alignment: .trailing, spacing: 4) {
        if let title = turn.pageTitle {
          Label(title, systemImage: "doc.text").font(.caption).foregroundStyle(.secondary).lineLimit(1)
        }
        Text(turn.text)
          .padding(.horizontal, 12)
          .padding(.vertical, 8)
          .background(Color.accentColor.opacity(0.15), in: RoundedRectangle(cornerRadius: 16))
          .textSelection(.enabled)
      }
      .frame(maxWidth: .infinity, alignment: .trailing)
    } else {
      VStack(alignment: .leading, spacing: 8) {
        ForEach(Array(turn.content.enumerated()), id: \.offset) { _, block in
          switch block["type"]?.string {
          case "thinking":
            if let thinking = block["thinking"]?.string, !thinking.isEmpty {
              ThinkingView(text: thinking, live: turn.streaming && block["signature"] == nil)
            }
          case "text":
            if let text = block["text"]?.string, !text.isEmpty {
              ClaudeMarkdownView(text: text)
            }
          case "tool_use":
            if let id = block["id"]?.string {
              if let view = tools[id], let change = view.memory {
                MemoryRow(change: change, undone: view.memoryUndone) { undoMemory(id) }
              } else if let view = tools[id], let change = view.boost {
                BoostRow(change: change, undone: view.boostUndone) { undoBoost(id) }
              } else {
                ToolRow(name: block["name"]?.string ?? "", input: block["input"], view: tools[id])
              }
            }
          default:
            EmptyView()
          }
        }
        if turn.streaming && !turn.content.contains(where: { $0["type"]?.string == "text" }) {
          ProgressView().controlSize(.small)
        }
        switch turn.stopReason {
        case "refusal": notice("Claude declined to answer this.")
        case "max_tokens": notice("The answer was cut off at its length limit.")
        default: EmptyView()
        }
        if let error = turn.error {
          HStack(alignment: .firstTextBaseline) {
            Text(error).foregroundStyle(.red)
            Button("Retry", action: retry)
          }
          .font(.callout)
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading)
    }
  }

  private func notice(_ text: LocalizedStringKey) -> some View {
    Text(text).font(.callout).foregroundStyle(.secondary)
  }
}

/// Claude's thinking, summarized: one line while it thinks, the summary on
/// a tap.
private struct ThinkingView: View {
  let text: String
  let live: Bool
  @State private var open = false

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      Button {
        withAnimation(.snappy) { open.toggle() }
      } label: {
        Label(live ? LocalizedStringKey("Thinking…") : "Thought", systemImage: open ? "chevron.down" : "chevron.right")
          .font(.caption.weight(.medium))
          .foregroundStyle(.secondary)
      }
      .buttonStyle(.plain)
      if open {
        Text(text).font(.caption).foregroundStyle(.secondary).textSelection(.enabled)
      }
    }
  }
}

/// What a Boost tool changed, with Undo (docs/design/boosts.md): "Saved
/// Boost for github.com".
private struct BoostRow: View {
  let change: ClaudeTools.BoostChange
  let undone: Bool
  let undo: () -> Void

  var body: some View {
    HStack(alignment: .firstTextBaseline, spacing: 6) {
      Image(systemName: change.kind == "deleted" ? "trash" : "wand.and.stars")
      Text(line)
        .strikethrough(undone)
        .lineLimit(2)
      Spacer(minLength: 0)
      if undone {
        Text("Undone")
      } else {
        Button("Undo", action: undo).font(.caption.weight(.medium))
      }
    }
    .font(.caption)
    .foregroundStyle(.secondary)
  }

  private var line: String {
    let site = change.boost.parsedMatch?.site ?? change.boost.match
    let name = change.boost.name
    return switch change.kind {
    case "deleted":
      String(
        localized: "Deleted Boost for \(site): \(name)",
        comment: "Claude chat: a Boost Claude deleted; the arguments are its site and name.")
    case "updated":
      String(
        localized: "Updated Boost for \(site): \(name)",
        comment: "Claude chat: a Boost Claude changed; the arguments are its site and name.")
    default:
      String(
        localized: "Saved Boost for \(site): \(name)",
        comment: "Claude chat: a Boost Claude made; the arguments are its site and name.")
    }
  }
}

/// What a memory tool changed, with Undo (docs/design/claude.md, "Memory").
private struct MemoryRow: View {
  let change: ClaudeTools.MemoryChange
  let undone: Bool
  let undo: () -> Void

  var body: some View {
    HStack(alignment: .firstTextBaseline, spacing: 6) {
      Image(systemName: change.kind == "forgot" ? "bookmark.slash" : "bookmark")
      Text(line)
        .strikethrough(undone)
        .lineLimit(2)
      Spacer(minLength: 0)
      if undone {
        Text("Undone")
      } else {
        Button("Undo", action: undo).font(.caption.weight(.medium))
      }
    }
    .font(.caption)
    .foregroundStyle(.secondary)
  }

  private var line: String {
    let text = change.text
    return switch change.kind {
    case "forgot":
      String(localized: "Forgot: \(text)", comment: "Claude chat: a fact Claude removed from its memory.")
    case "updated":
      String(localized: "Updated memory: \(text)", comment: "Claude chat: a fact Claude changed in its memory.")
    default:
      String(localized: "Saved to memory: \(text)", comment: "Claude chat: a fact Claude saved to its memory.")
    }
  }
}

/// A tool call in a reply: what Claude did, and how it went; the result
/// (and a screenshot) on a tap.
private struct ToolRow: View {
  let name: String
  let input: JSONValue?
  let view: ClaudeChat.ToolView?
  @State private var open = false

  var body: some View {
    VStack(alignment: .leading, spacing: 6) {
      Button {
        withAnimation(.snappy) { open.toggle() }
      } label: {
        HStack(spacing: 6) {
          status
          Text(title).font(.caption.weight(.medium)).lineLimit(1)
          Spacer(minLength: 0)
          Image(systemName: open ? "chevron.down" : "chevron.right").font(.caption2)
        }
        .foregroundStyle(.secondary)
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      if let image = view?.image {
        Image(uiImage: image)
          .resizable()
          .scaledToFit()
          .frame(maxHeight: open ? 400 : 90, alignment: .leading)
          .clipShape(RoundedRectangle(cornerRadius: 8))
      }
      if open, let text = view?.text, !text.isEmpty {
        Text(text.count > 3000 ? String(text.prefix(3000)) + "…" : text)
          .font(.caption.monospaced())
          .foregroundStyle(view?.isError == true ? .red : .secondary)
          .textSelection(.enabled)
      }
    }
    .padding(8)
    .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 10))
  }

  @ViewBuilder private var status: some View {
    if view == nil || view?.running == true {
      ProgressView().controlSize(.mini)
    } else if view?.isError == true {
      Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
    } else {
      Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
    }
  }

  /// "Click e12", "Navigate example.com": the tool and its main argument.
  private var title: String {
    let label = name.replacingOccurrences(of: "_", with: " ").capitalized(with: nil)
    let detail = ["ref", "url", "query", "text", "keys", "direction", "code"].lazy.compactMap { input?[$0]?.string }.first
      ?? input?["coordinate"]?.array.map { $0.compactMap(\.number).map { String(Int($0)) }.joined(separator: ", ") }
    guard let detail, !detail.isEmpty else { return label }
    return "\(label) \(detail.count > 60 ? String(detail.prefix(60)) + "…" : detail)"
  }
}
