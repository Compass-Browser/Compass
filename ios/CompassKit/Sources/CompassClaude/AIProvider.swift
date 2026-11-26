// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// Who answers the chat and the sidebar's requests (docs/design/
/// ai-providers.md). Anthropic speaks the Messages API, as Compass does
/// inside; OpenAI gets the Responses API (`OpenAIResponses`), the others
/// Chat Completions (`OpenAIChat`), translated at the edge.
public enum AIProvider: String, CaseIterable, Sendable, Identifiable {
  case anthropic
  case openAI = "openai"
  case gemini
  /// Any server that speaks OpenAI's Chat Completions, at its base URL.
  case compatible

  public var id: String { rawValue }

  public var displayName: String {
    switch self {
    case .anthropic: "Anthropic"
    case .openAI: "OpenAI"
    case .gemini: "Google Gemini"
    case .compatible: String(localized: "Other (OpenAI-Compatible)", bundle: .module, comment: "AI provider choice in Settings → AI: any server speaking OpenAI's API.")
    }
  }

  /// What the UI calls the assistant: "Ask Claude", "Ask GPT".
  public var assistantName: String {
    switch self {
    case .anthropic: "Claude"
    case .openAI: "GPT"
    case .gemini: "Gemini"
    case .compatible: "AI"
    }
  }

  /// "Ask Claude", "Ask GPT", "Ask Gemini", "Ask AI".
  public var askTitle: String {
    String(localized: "Ask \(assistantName)", bundle: .module, comment: "Command bar row, page menu and chat field: Ask Claude, Ask GPT, Ask Gemini, Ask AI. The argument is the assistant's name.")
  }

  /// Chat Completions rather than the Messages API.
  public var usesOpenAIFormat: Bool { self != .anthropic }

  /// The base URL when the user gives none; nil when one is needed.
  public var defaultBaseURL: URL? {
    switch self {
    case .anthropic: URL(string: "https://api.anthropic.com")!
    case .openAI: URL(string: "https://api.openai.com/v1")!
    case .gemini: URL(string: "https://generativelanguage.googleapis.com/v1beta/openai")!
    case .compatible: nil
    }
  }

  /// What a key looks like, for the field's placeholder.
  public var keyPlaceholder: String {
    switch self {
    case .anthropic: "sk-ant-…"
    case .openAI: "sk-…"
    case .gemini: "AIza…"
    case .compatible: String(localized: "Key (if the server needs one)", bundle: .module, comment: "Placeholder of the API key field in Settings → AI, for an OpenAI-compatible server.")
    }
  }

  /// The chat's model until the user picks one; empty for `compatible`,
  /// whose models only its server knows.
  public var defaultModel: String {
    switch self {
    case .anthropic: ClaudeEndpoint.defaultModel
    case .openAI: "gpt-6.1-terra"
    case .gemini: "gemini-2.5-pro"
    case .compatible: ""
    }
  }

  /// The sidebar's small requests' model; empty is the chat's.
  public var defaultQuickModel: String {
    switch self {
    case .anthropic: "claude-haiku-5-5"
    case .openAI: "gpt-6-luna"
    case .gemini: "gemini-2.5-flash"
    case .compatible: ""
    }
  }

  /// Ask in the command bar's model; empty is the chat's.
  public var defaultAskModel: String {
    switch self {
    case .anthropic: "claude-sonnet-5-5"
    case .openAI: "gpt-6-luna"
    case .gemini: "gemini-2.5-flash"
    case .compatible: ""
    }
  }

  /// Offered until "Test Connection" lists the key's own.
  public var knownModels: [ClaudeModel] {
    switch self {
    case .anthropic: ClaudeModel.known
    case .openAI:
      [
        ClaudeModel(id: "gpt-6.2-astra", displayName: "GPT-6.2 Astra"),
        ClaudeModel(id: "gpt-6.2-sol", displayName: "GPT-6.2 Sol"),
        ClaudeModel(id: "gpt-6.1-terra", displayName: "GPT-6.1 Terra"),
        ClaudeModel(id: "gpt-6-luna", displayName: "GPT-6 Luna"),
      ]
    case .gemini:
      [
        ClaudeModel(id: "gemini-2.5-pro", displayName: "Gemini 2.5 Pro"),
        ClaudeModel(id: "gemini-2.5-flash", displayName: "Gemini 2.5 Flash"),
        ClaudeModel(id: "gemini-2.5-flash-lite", displayName: "Gemini 2.5 Flash-Lite"),
      ]
    case .compatible: []
    }
  }

  /// Where it says what was sent goes (Settings).
  public var privacyNote: String {
    switch self {
    case .anthropic: String(localized: "Chats and the sidebar's requests go to Anthropic, or the endpoint under Advanced.", bundle: .module, comment: "Settings → AI: privacy note for the Anthropic provider.")
    case .openAI: String(localized: "Chats and the sidebar's requests go to OpenAI, under its terms, instead of Anthropic.", bundle: .module, comment: "Settings → AI: privacy note for the OpenAI provider.")
    case .gemini: String(localized: "Chats and the sidebar's requests go to Google, under the Gemini API's terms, instead of Anthropic.", bundle: .module, comment: "Settings → AI: privacy note for the Google Gemini provider.")
    case .compatible: String(localized: "Chats and the sidebar's requests go to the server below, instead of Anthropic.", bundle: .module, comment: "Settings → AI: privacy note for an OpenAI-compatible server.")
    }
  }
}
