// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassClaude
import SwiftUI

/// Settings → Claude, the desktop's Claude settings page: the API key
/// (only its hint shows once saved), the model, Test Connection, the
/// whole-browser permission, and under Advanced a compatible endpoint and
/// its auth style.
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
