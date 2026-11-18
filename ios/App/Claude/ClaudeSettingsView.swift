// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import CompassClaude
import SwiftUI
import UniformTypeIdentifiers

/// Settings → AI, the desktop's Claude settings page: the API key
/// (only its hint shows once saved), the model, Test Connection, the
/// whole-browser permission, memory, and under Advanced a compatible
/// endpoint and its auth style. The provider comes first
/// (docs/design/ai-providers.md): each keeps its own key and models.
struct ClaudeSettingsView: View {
  @State private var keyField = ""
  @State private var testing = false
  @State private var testResult: String?
  @State private var testOK = false
  @State private var showingAdvanced = false
  @State private var pickingFolder = false
  @State private var folderError: String?

  private var settings: ClaudeSettings { .shared }

  var body: some View {
    @Bindable var settings = settings
    Form {
      Section {
        Picker("Provider", selection: $settings.provider) {
          ForEach(AIProvider.allCases) { Text($0.displayName).tag($0) }
        }
        .accessibilityIdentifier("AIProvider")
        .onChange(of: settings.provider) {
          keyField = ""
          testResult = nil
        }
      } footer: {
        Text(settings.provider.privacyNote)
      }

      if settings.provider == .compatible {
        Section {
          TextField("https://example.com/v1", text: $settings.baseURLText)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .keyboardType(.URL)
            .onSubmit { settings.endpointChanged() }
          if !settings.baseURLIsValid {
            Text("Use https (http only for this device), with no query or credentials.")
              .font(.caption).foregroundStyle(.red)
          }
          TextField("Model ID", text: $settings.model)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
        } header: {
          Text("Server")
        } footer: {
          Text("The base URL its API answers under, before chat/completions (OpenRouter, Ollama, LM Studio, a company gateway), and the model to ask.")
        }
      }

      Section {
        if let hint = settings.keyHint {
          LabeledContent("API Key", value: hint)
          Button("Remove Key", role: .destructive) {
            settings.removeKey()
            testResult = nil
          }
        } else {
          SecureField(settings.provider.keyPlaceholder, text: $keyField)
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
        Text(keyFooter)
      }

      Section {
        // An OpenAI-compatible server's model is typed above; once Test
        // Connection lists its models, they can be picked too.
        if settings.provider != .compatible || (!settings.models.isEmpty && !settings.model.isEmpty) {
          Picker("Model", selection: $settings.model) {
            ForEach(settings.pickerModels) { model in
              Text(model.displayName == model.id ? model.id : model.displayName).tag(model.id)
            }
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
        .disabled((!settings.hasKey && settings.provider != .compatible) || testing || settings.endpoint == nil)
        if let testResult {
          Label(testResult, systemImage: testOK ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
            .foregroundStyle(testOK ? .green : .red)
            .font(.callout)
        }
      }

      if settings.provider == .anthropic {
      Section {
        Toggle("Accept the Risks of Subscription Tokens", isOn: $settings.acceptTokenRisks)
      } footer: {
        Text("Lets the key be a token from `claude setup-token` (sk-ant-oat…), which uses your Claude subscription. Compass then sends it as Claude Code does, saying it's Claude Code. Anthropic doesn't offer these tokens for other apps: they can stop working at any time, and using them this way may go against the terms of your subscription. Off, such a token is refused.")
      }
      }

      Section {
        Toggle("Let AI Use the Whole Browser", isOn: $settings.browserScope)
      } footer: {
        Text("Off: the AI only works with the page you're chatting about. On: it can also read and open your other tabs. Web pages can contain text written to trick it, so turn this on only when you need it.")
      }

      Section {
        Toggle("Let AI Use Developer Tools", isOn: $settings.devtools)
      } footer: {
        Text("The AI can inspect the page it works on as a web developer would: its requests and their answers, styles, cookies and storage, and performance, and try it as another device. Changes to cookies or storage ask you first. While on, Compass records the pages' requests and passes your profiles' connections through a proxy in the app, which sees where they go but never what they carry. Web pages can contain text written to trick it, so turn this on only when you need it.")
      }

      Section {
        Toggle("Let AI Make Boosts", isOn: $settings.boosts)
          .accessibilityIdentifier("ClaudeBoosts")
        NavigationLink { BoostsView() } label: {
          LabeledContent("Boosts", value: "\(BoostStore.shared.boosts.count)")
        }
      } footer: {
        Text("The AI can restyle a site for you with a Boost, CSS that applies each time you visit, and change the Boosts it made. CSS saves at once, with Undo in the chat. JavaScript always asks you first, showing the code. Boosts sync with your spaces, end-to-end encrypted.")
      }

      // Claude in the sidebar (docs/design/ai-sidebar.md): a row per
      // feature, each with its own switch and model.
      Section {
        ClaudeFeatureRow(
          title: "Name Link Folders",
          description:
            "The AI names the folders that gather a page's links, from their tabs' titles and sites, and folder menus offer “Suggest a Name”. Each new name can be undone from the folder's menu.",
          isOn: $settings.nameLinkFolders, model: $settings.nameLinkFoldersModel,
          models: settings.pickerModels(including: settings.nameLinkFoldersModel), ready: settings.isSetUp,
          identifier: "ClaudeNameLinkFolders")
        ClaudeFeatureRow(
          title: "Tidy Today",
          description:
            "Today's header offers “Tidy”: the AI groups Today's loose tabs into named folders, from their titles and sites. “Undo” puts everything back.",
          isOn: $settings.tidyToday, model: $settings.tidyTodayModel,
          models: settings.pickerModels(including: settings.tidyTodayModel), ready: settings.isSetUp,
          identifier: "ClaudeTidyToday")
        ClaudeFeatureRow(
          title: "Shorter Tab Titles",
          description:
            "Long tab titles read as short ones in the sidebar, “PR #123: fix login” for a pull request's whole title. Only in the sidebar's rows, never saved or synced; a name you give a tab wins.",
          isOn: $settings.shortTabTitles, model: $settings.shortTabTitlesModel,
          models: settings.pickerModels(including: settings.shortTabTitlesModel), ready: settings.isSetUp,
          identifier: "ClaudeShortTabTitles")
        ClaudeFeatureRow(
          title: "Answer Questions in the Command Bar",
          description:
            "A question typed in the command bar gets an “\(settings.provider.askTitle)” row, and a short answer under the field; “Continue in Chat” goes on from there. “About this page” sends the page's title and site, never what it contains.",
          isOn: $settings.commandBarAsk, model: $settings.commandBarAskModel,
          models: settings.pickerModels(including: settings.commandBarAskModel), ready: settings.isSetUp,
          identifier: "ClaudeCommandBarAsk")
      } header: {
        Text("AI in the Sidebar")
      } footer: {
        Text(
          "Small tasks the AI does for you, each with its own switch and model. Only tabs' titles and sites are sent (and the questions you ask), to the endpoint above, never what pages contain; Incognito never. At most 30 automatic requests an hour."
        )
      }

      Section {
        Toggle("Use Memory", isOn: $settings.memoryEnabled)
        if settings.memoryEnabled {
          Toggle("Let AI Save Memories", isOn: $settings.memorySave)
          NavigationLink {
            ClaudeMemoryView()
          } label: {
            LabeledContent("Memories", value: "\(ClaudeMemoryStore.shared.entries.count)")
          }
        }
      } header: {
        Text("Memory")
      } footer: {
        Text("The AI keeps short facts about you, like how you like answers, and starts each new chat with them. Each save shows in the chat, with Undo. Memories sync with your spaces, end-to-end encrypted.")
      }

      Section {
        Picker("AI Can", selection: $settings.fileAccess) {
          Text("Not Use Files").tag(ClaudeFileAccess.off)
          Text("Save Files").tag(ClaudeFileAccess.save)
          Text("Use a Folder").tag(ClaudeFileAccess.folder)
        }
        .accessibilityIdentifier("ClaudeFilesAccess")
        if settings.fileAccess != .off {
          LabeledContent(
            "Folder",
            value: settings.folderName
              ?? (settings.fileAccess == .save ? ClaudeSettings.documentsName : "None"))
          Button(settings.folderName == nil ? "Choose a Folder…" : "Choose Another Folder…") { pickingFolder = true }
          if settings.folderName != nil, settings.fileAccess == .save {
            Button("Use Downloads") { settings.clearFolder() }
          }
          if let folderError {
            Text(folderError).font(.caption).foregroundStyle(.red)
          }
        }
      } header: {
        Text("Files")
      } footer: {
        Text(filesFooter)
      }

      if settings.provider == .anthropic {
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
    }
    .navigationTitle("AI")
    .fileImporter(isPresented: $pickingFolder, allowedContentTypes: [.folder]) { result in
      switch result {
      case .success(let url):
        folderError = settings.setFolder(url) ? nil : "Compass can't keep access to that folder."
      case .failure(let error):
        folderError = error.localizedDescription
      }
    }
  }

  private var keyFooter: String {
    switch settings.provider {
    case .anthropic:
      return "An Anthropic API key from console.anthropic.com, or a key for the endpoint under Advanced. It stays in this device's Keychain. A token from `claude setup-token` uses your Claude subscription, only with the risks below accepted."
    case .openAI:
      return "An OpenAI API key from platform.openai.com (a ChatGPT subscription can't be used). It stays in this device's Keychain. Compass's prompts and tools are made for Claude: other models may use the browser less well."
    case .gemini:
      return "A Gemini API key from aistudio.google.com. It stays in this device's Keychain. Compass's prompts and tools are made for Claude: other models may use the browser less well."
    case .compatible:
      return "The server's key, if it needs one. It stays in this device's Keychain."
    }
  }

  private var filesFooter: String {
    switch settings.fileAccess {
    case .off:
      return "The AI can't save, read or write files."
    case .save:
      return "The AI can save new files it makes for you, like notes or a table, in the folder; it never replaces one. You see each file and allow it first."
    case .folder:
      return settings.folderName == nil
        ? "Choose the folder the AI may use."
        : "The AI can list and read the files in the folder, and save and change them after you allow each one. Files can contain text written to trick it: use a folder with only what the task needs."
    }
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

/// One of Claude's features in the sidebar (Settings → AI): its own
/// switch and the model it asks, both disabled, with a line saying why,
/// while Claude isn't set up. A new feature adds a row.
struct ClaudeFeatureRow: View {
  let title: String
  let description: String
  @Binding var isOn: Bool
  @Binding var model: String
  /// The chat picker's models (a gateway's list too).
  let models: [ClaudeModel]
  /// Claude is set up (a key saved).
  let ready: Bool
  let identifier: String

  var body: some View {
    Toggle(isOn: Binding(get: { isOn && ready }, set: { isOn = $0 })) {
      VStack(alignment: .leading, spacing: 2) {
        Text(title)
        Text(description).font(.caption).foregroundStyle(.secondary)
      }
    }
    .disabled(!ready)
    .accessibilityIdentifier(identifier)
    Picker("Model", selection: $model) {
      ForEach(models) { model in
        Text(model.displayName == model.id ? model.id : model.displayName).tag(model.id)
      }
    }
    .disabled(!ready)
    .accessibilityIdentifier(identifier + "Model")
    if !ready {
      Label("Needs AI set up: save an API key (or a server) above.", systemImage: "info.circle")
        .font(.caption)
        .foregroundStyle(.secondary)
    }
  }
}

/// Settings → AI → Memories: every fact Claude keeps, to edit (tap),
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
      "Delete everything the AI remembers about you, on all your devices?", isPresented: $confirmingDeleteAll,
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
