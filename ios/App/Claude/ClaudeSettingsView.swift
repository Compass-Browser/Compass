// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import SwiftUI

/// Settings → Claude, the desktop's Claude settings page: the API key
/// (only its hint shows once saved), the model, Test Connection, the
/// whole-browser permission, memory, and under Advanced a compatible
/// endpoint and its auth style.
struct ClaudeSettingsView: View {
  @State private var keyField = ""
  @State private var testing = false
  @State private var testResult: String?
  @State private var testOK = false
  @State private var showingAdvanced = false

  private var settings: ClaudeSettings { .shared }

  var body: some View {
    @Bindable var settings = settings
    Form {
      Section {
        if let hint = settings.keyHint {
          LabeledContent("API Key", value: hint)
          Button("Remove Key", role: .destructive) {
            settings.removeKey()
            testResult = nil
          }
        } else {
          SecureField("sk-ant-…", text: $keyField)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .accessibilityIdentifier("ClaudeKeyField")
          Button("Save Key") {
            settings.setKey(keyField)
            keyField = ""
          }
          .disabled(keyField.trimmingCharacters(in: .whitespaces).isEmpty)
        }
      } header: {
        Text("API Key")
      } footer: {
        Text("An Anthropic API key from console.anthropic.com, or a key for the endpoint under Advanced. It stays in this device's Keychain. A token from `claude setup-token` uses your Claude subscription, only with the risks below accepted.")
      }

      Section {
        Picker("Model", selection: $settings.model) {
          ForEach(settings.pickerModels) { model in
            Text(model.displayName == model.id ? model.id : model.displayName).tag(model.id)
          }
        }
        Button {
          test()
        } label: {
          HStack {
            Text("Test Connection")
            Spacer()
            if testing { ProgressView() }
          }
        }
        .disabled(!settings.hasKey || testing || !settings.baseURLIsValid)
        if let testResult {
          Label(testResult, systemImage: testOK ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
            .foregroundStyle(testOK ? .green : .red)
            .font(.callout)
        }
      }

      Section {
        Toggle("Accept the Risks of Subscription Tokens", isOn: $settings.acceptTokenRisks)
      } footer: {
        Text("Lets the key be a token from `claude setup-token` (sk-ant-oat…), which uses your Claude subscription. Compass then sends it as Claude Code does, saying it's Claude Code. Anthropic doesn't offer these tokens for other apps: they can stop working at any time, and using them this way may go against the terms of your subscription. Off, such a token is refused.")
      }

      Section {
        Toggle("Let Claude Use the Whole Browser", isOn: $settings.browserScope)
      } footer: {
        Text("Off: Claude only works with the page you're chatting about. On: it can also read and open your other tabs. Web pages can contain text written to trick Claude, so turn this on only when you need it.")
      }

      Section {
        Toggle("Use Memory", isOn: $settings.memoryEnabled)
        if settings.memoryEnabled {
          Toggle("Let Claude Save Memories", isOn: $settings.memorySave)
          NavigationLink {
            ClaudeMemoryView()
          } label: {
            LabeledContent("Memories", value: "\(ClaudeMemoryStore.shared.entries.count)")
          }
        }
      } header: {
        Text("Memory")
      } footer: {
        Text("Claude keeps short facts about you, like how you like answers, and starts each new chat with them. Each save shows in the chat, with Undo. Memories sync with your spaces, end-to-end encrypted.")
      }

      Section {
        DisclosureGroup("Advanced", isExpanded: $showingAdvanced) {
          TextField("https://api.anthropic.com", text: $settings.baseURLText)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .keyboardType(.URL)
            .onSubmit { settings.endpointChanged() }
          if !settings.baseURLIsValid {
            Text("Use https (http only for this device), with no query or credentials.")
              .font(.caption).foregroundStyle(.red)
          }
          Picker("Send the Key As", selection: $settings.authStyle) {
            Text("x-api-key").tag(ClaudeAuthStyle.apiKey)
            Text("Authorization: Bearer").tag(ClaudeAuthStyle.bearer)
          }
          .onChange(of: settings.authStyle) { settings.endpointChanged() }
        }
      } footer: {
        Text("For Amazon Bedrock, Google Vertex AI or a compatible gateway. Empty is the Claude API.")
      }
    }
    .navigationTitle("Claude")
  }

  private func test() {
    guard let endpoint = settings.endpoint else { return }
    if endpoint.needsTokenRisksAccepted {
      testOK = false
      testResult = ClaudeEndpoint.tokenRisksMessage
      return
    }
    testing = true
    testResult = nil
    Task {
      let result = await ClaudeClient.models(endpoint: endpoint)
      testing = false
      switch result {
      case .success(let models):
        testOK = true
        testResult = "Connected: \(models.count) model\(models.count == 1 ? "" : "s") available."
        if !models.isEmpty { settings.models = models }
      case .failure(let error):
        testOK = false
        testResult = error.message
      }
    }
  }
}

/// Settings → Claude → Memories: every fact Claude keeps, to edit (tap),
/// delete (swipe) or add; Delete All.
struct ClaudeMemoryView: View {
  @State private var newText = ""
  @State private var editing: MemoryEntry?
  @State private var editText = ""
  @State private var error: String?
  @State private var confirmingDeleteAll = false

  private var memory: ClaudeMemoryStore { .shared }

  var body: some View {
    List {
      Section {
        ForEach(memory.entries) { entry in
          Button {
            editText = entry.text
            editing = entry
          } label: {
            Text(entry.text).foregroundStyle(.primary)
          }
        }
        .onDelete { offsets in
          for id in offsets.map({ memory.entries[$0].id }) { memory.remove(id) }
        }
        if memory.entries.isEmpty {
          Text("No memories yet.").foregroundStyle(.secondary)
        }
      } footer: {
        if let error { Text(error).foregroundStyle(.red) }
      }

      Section {
        TextField("Add a fact, like “I live in Milan”", text: $newText)
          .onSubmit(add)
        Button("Add", action: add).disabled(newText.trimmingCharacters(in: .whitespaces).isEmpty)
      }

      if !memory.entries.isEmpty {
        Section {
          Button("Delete All Memories", role: .destructive) { confirmingDeleteAll = true }
        }
      }
    }
    .navigationTitle("Memories")
    .confirmationDialog(
      "Delete everything Claude remembers about you, on all your devices?", isPresented: $confirmingDeleteAll,
      titleVisibility: .visible
    ) {
      Button("Delete All", role: .destructive) { memory.removeAll() }
    }
    .alert("Edit Memory", isPresented: Binding(get: { editing != nil }, set: { if !$0 { editing = nil } })) {
      TextField("Memory", text: $editText)
      Button("Save") {
        if let editing { report(memory.update(editing.id, text: editText)) }
        editing = nil
      }
      Button("Cancel", role: .cancel) { editing = nil }
    }
  }

  private func add() {
    guard !newText.trimmingCharacters(in: .whitespaces).isEmpty else { return }
    if report(memory.add(newText)) { newText = "" }
  }

  @discardableResult
  private func report(_ result: Result<MemoryEntry, MemoryList.Failure>) -> Bool {
    switch result {
    case .success:
      error = nil
      return true
    case .failure(let failure):
      error = failure.description
      return false
    }
  }
}
