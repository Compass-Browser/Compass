// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import SwiftUI

/// The Claude chat sheet (docs/design/claude.md, "iOS"): at the medium
/// detent the page stays visible above it. The window's conversation, a
/// field to write in, Stop while Claude answers, New Chat, and the page
/// attachment.
struct ClaudeChatSheet: View {
  @Environment(AppModel.self) private var app
  @State private var text = ""
  @State private var showingSettings = false
  @FocusState private var focused: Bool

  private var chat: ClaudeChat { app.claude }
  private var settings: ClaudeSettings { .shared }

  var body: some View {
    NavigationStack {
      Group {
        if !settings.hasKey {
          noKey
        } else if chat.isEmpty {
          emptyState
        } else {
          messages
        }
      }
      .safeAreaInset(edge: .bottom) { if settings.hasKey { composer } }
      .navigationTitle("Claude")
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
            TurnView(turn: turn) { chat.retry() }.id(turn.id)
          }
          Color.clear.frame(height: 1).id("end")
        }
        .padding()
      }
      .scrollDismissesKeyboard(.interactively)
      .onChange(of: chat.conversation.turns.last?.content.count) { proxy.scrollTo("end", anchor: .bottom) }
      .onChange(of: chat.conversation.turns.count) { withAnimation { proxy.scrollTo("end", anchor: .bottom) } }
    }
  }

  private var composer: some View {
    VStack(spacing: 6) {
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
        .accessibilityLabel(chat.attachPage ? "Page attached" : "Page not attached")
        .frame(maxWidth: .infinity, alignment: .leading)
      }
      HStack(alignment: .bottom, spacing: 8) {
        TextField("Ask Claude", text: $text, axis: .vertical)
          .lineLimit(1...6)
          .focused($focused)
          .submitLabel(.send)
          .onSubmit(send)
          .padding(.horizontal, 12)
          .padding(.vertical, 8)
          .background(.quaternary, in: RoundedRectangle(cornerRadius: 18))
          .accessibilityIdentifier("ClaudeInput")
        if chat.streaming {
          Button { chat.stop() } label: { Image(systemName: "stop.circle.fill").font(.title) }
            .accessibilityLabel("Stop")
        } else {
          Button(action: send) { Image(systemName: "arrow.up.circle.fill").font(.title) }
            .disabled(text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            .accessibilityLabel("Send")
        }
      }
    }
    .padding(.horizontal)
    .padding(.vertical, 8)
    .background(.bar)
  }

  private func send() {
    chat.send(text, app: app)
    text = ""
  }
}

/// One turn: the user's message (with the page it carried), or Claude's
/// reply: its thinking summary, its text, and how it ended.
private struct TurnView: View {
  let turn: Conversation.Turn
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
              Text(Self.markdown(text)).textSelection(.enabled)
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

  private func notice(_ text: String) -> some View {
    Text(text).font(.callout).foregroundStyle(.secondary)
  }

  /// Inline markdown (bold, italics, code, links), line breaks kept.
  static func markdown(_ text: String) -> AttributedString {
    (try? AttributedString(markdown: text, options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)))
      ?? AttributedString(text)
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
        Label(live ? "Thinking…" : "Thought", systemImage: open ? "chevron.down" : "chevron.right")
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
