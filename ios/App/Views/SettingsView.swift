// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassSync
import LocalAuthentication
import SwiftUI

struct SettingsView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State private var showingRecovery = false
  @State private var confirmLeave = false
  @State private var confirmClear = false

  var body: some View {
    NavigationStack {
      Form {
        if app.sync.isOn { syncOn } else { syncOff }
        Section {
          NavigationLink("Passwords") { PasswordsView() }
        }
        Section("Privacy") {
          Button("Clear History", role: .destructive) { confirmClear = true }
        }
      }
      .confirmationDialog("Clear all history?", isPresented: $confirmClear) {
        Button("Clear History", role: .destructive) { app.sync.clearHistory() }
      } message: {
        Text(app.sync.isOn ? "History is cleared on every synced device." : "History on this iPhone is cleared.")
      }
      .navigationTitle("Settings")
      .toolbar { Button("Done") { dismiss() } }
      // A pairing link from the camera goes straight to joining.
      .navigationDestination(
        isPresented: Binding(get: { app.pendingInvite != nil && !app.sync.isOn }, set: { _ in })
      ) { JoinView() }
    }
  }

  @ViewBuilder private var syncOff: some View {
    Section {
      NavigationLink("Join with a Mac") { JoinView() }
    } header: {
      Text("Sync")
    } footer: {
      Text("On your Mac, open Compass's sync settings and choose “Add a device”. Your data is encrypted on your devices; the server can't read it.")
    }
  }

  @ViewBuilder private var syncOn: some View {
    Section("Sync") {
      LabeledContent("Status", value: status)
      if let server = app.sync.serverURL { LabeledContent("Server", value: server.host() ?? server.absoluteString) }
      if let last = app.sync.lastSync {
        LabeledContent("Last synced") { Text(last, style: .relative) }
      }
      Button("Sync Now") { Task { await app.sync.syncNow() } }
    }
    Section("Devices") {
      ForEach(app.sync.devices) { device in
        HStack {
          Image(systemName: device.formFactor == .phone ? "iphone" : device.formFactor == .tablet ? "ipad" : "laptopcomputer")
          Text(device.name)
          if device.isThisDevice { Text("This device").font(.caption).foregroundStyle(.secondary) }
          Spacer()
          Text(device.lastUpdated, style: .date).font(.caption).foregroundStyle(.secondary)
        }
      }
    }
    Section {
      Button("Show Recovery Code") { authenticate() }
      Button("Leave Sync", role: .destructive) { confirmLeave = true }
    } footer: {
      Text("The recovery code adds a device when no synced device is at hand. Keep it somewhere safe.")
    }
    .sheet(isPresented: $showingRecovery) { RecoveryCodeView(words: app.sync.recoveryWords) }
    .confirmationDialog("Leave sync?", isPresented: $confirmLeave) {
      Button("Leave Sync", role: .destructive) { app.sync.leave() }
    } message: {
      Text("Your spaces stay on this iPhone. Other devices keep syncing.")
    }
  }

  private var status: String {
    switch app.sync.state {
    case .off: "Off"
    case .syncing: "Syncing…"
    case .on: "On"
    case .failed(let message): message
    }
  }

  private func authenticate() {
    let context = LAContext()
    var error: NSError?
    guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else {
      showingRecovery = true  // no passcode set: nothing to ask for
      return
    }
    context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: "Show your sync recovery code") { ok, _ in
      if ok { Task { @MainActor in showingRecovery = true } }
    }
  }
}

struct RecoveryCodeView: View {
  @Environment(\.dismiss) private var dismiss
  let words: [String]

  var body: some View {
    NavigationStack {
      VStack(spacing: 20) {
        LazyVGrid(columns: [GridItem(), GridItem(), GridItem()], spacing: 12) {
          ForEach(Array(words.enumerated()), id: \.offset) { i, word in
            Text("\(i + 1). \(word)").font(.body.monospaced()).frame(maxWidth: .infinity, alignment: .leading)
          }
        }
        .padding()
        .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 12))
        Text("Anyone with these words can read your synced data. If you lose every device and these words, the synced data is gone.")
          .font(.footnote).foregroundStyle(.secondary)
        Spacer()
      }
      .padding()
      .navigationTitle("Recovery Code")
      .toolbar { Button("Done") { dismiss() } }
    }
  }
}

/// Joining a chain: the code a Mac shows (typed, or scanned from its QR),
/// or the 12-word recovery code.
struct JoinView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State private var server = SyncController.defaultServer.absoluteString
  @State private var code = ""
  @State private var words = ""
  @State private var useRecovery = false
  @State private var working = false
  @State private var waitingForMac = false
  @State private var message: String?
  @State private var scanning = false

  var body: some View {
    Form {
      Section("Server") {
        TextField("https://sync.example.com/v2", text: $server)
          .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
      }
      if useRecovery {
        Section {
          TextField("12 words", text: $words, axis: .vertical)
            .textInputAutocapitalization(.never).autocorrectionDisabled()
          if let last = words.split(separator: " ").last, !words.hasSuffix(" ") {
            let suggestions = Bip39.words(withPrefix: String(last).lowercased(), max: 4)
            if !suggestions.isEmpty && !suggestions.contains(String(last)) {
              HStack {
                ForEach(suggestions, id: \.self) { word in
                  Button(word) {
                    words = words.split(separator: " ").dropLast().joined(separator: " ")
                    words += (words.isEmpty ? "" : " ") + word + " "
                  }
                  .buttonStyle(.bordered)
                }
              }
            }
          }
        } header: {
          Text("Recovery code")
        } footer: {
          Text(Bip39.parse(words) == nil && words.split(separator: " ").count == 12 ? "These words don't form a valid code." : "")
        }
      } else {
        Section {
          TextField("7-482913", text: $code).keyboardType(.numbersAndPunctuation)
          Button("Scan QR Code", systemImage: "qrcode.viewfinder") { scanning = true }
        } header: {
          Text("Code from your Mac")
        } footer: {
          Text("Shown under “Add a device” in Compass on your Mac.")
        }
      }
      Section {
        Button(waitingForMac ? "Confirm on your Mac…" : "Join") { join() }
          .disabled(working || URL(string: server) == nil)
        Button(useRecovery ? "Use a code from a Mac instead" : "Use a recovery code") { useRecovery.toggle() }
          .font(.footnote)
      } footer: {
        if let message { Text(message).foregroundStyle(.red) }
      }
    }
    .navigationTitle("Join Sync")
    .onAppear {
      if let invite = app.pendingInvite, code.isEmpty {
        server = invite.server.absoluteString
        code = invite.code.text
        join()
      }
    }
    .sheet(isPresented: $scanning) {
      QRScannerView { text in
        scanning = false
        if let url = URL(string: text), let invite = PairingInvite(url: url) {
          server = invite.server.absoluteString
          code = invite.code.text
          join()
        } else {
          message = "That QR code isn't a Compass pairing code."
        }
      }
    }
  }

  private func join() {
    guard let url = URL(string: server.trimmingCharacters(in: .whitespaces)) else { return }
    message = nil
    if useRecovery {
      do {
        try app.sync.join(recoveryCode: words, server: url)
        dismiss()
      } catch {
        message = "Check the 12 words."
      }
      return
    }
    working = true
    Task {
      let result = await app.sync.join(code: code, server: url) {
        Task { @MainActor in waitingForMac = true }
      }
      working = false
      waitingForMac = false
      app.pendingInvite = nil
      switch result {
      case .joined: dismiss()
      case .denied: message = "Your Mac said no."
      case .badCode: message = "A code looks like 7-482913."
      case .noSuchCode, .closed: message = "That code isn't open any more. Get a new one on your Mac."
      case .codeUsed: message = "That code was already used."
      case .wrongCode: message = "Wrong code. Your Mac shows a new one."
      case .canceled: message = "Canceled on your Mac."
      case .expired: message = "The code expired."
      case .rateLimited: message = "Too many tries. Wait a few minutes."
      case .failed: message = "Couldn't reach the server."
      }
    }
  }
}

struct PasswordsView: View {
  @Environment(AppModel.self) private var app
  @State private var query = ""
  @State private var unlocked = false

  var body: some View {
    List {
      if unlocked {
        ForEach(app.passwords.all.filter { query.isEmpty || $0.host.localizedCaseInsensitiveContains(query) || $0.username.localizedCaseInsensitiveContains(query) }) { p in
          NavigationLink {
            Form {
              LabeledContent("Website", value: p.host)
              if p.isBlocked {
                Text("Never saved for this site").foregroundStyle(.secondary)
              } else {
                LabeledContent("Username", value: p.username)
                LabeledContent("Password") { Text(p.password).textSelection(.enabled).font(.body.monospaced()) }
              }
              Button("Delete", role: .destructive) { app.passwords.delete(p) }
            }
            .navigationTitle(p.host)
          } label: {
            VStack(alignment: .leading) {
              Text(p.host)
              Text(p.isBlocked ? "Never saved" : p.username).font(.caption).foregroundStyle(.secondary)
            }
          }
        }
      } else {
        Button("Unlock Passwords") { unlock() }
      }
    }
    .searchable(text: $query)
    .navigationTitle("Passwords")
    .onAppear(perform: unlock)
  }

  private func unlock() {
    let context = LAContext()
    var error: NSError?
    guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else {
      unlocked = true
      return
    }
    context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: "Show your saved passwords") { ok, _ in
      if ok { Task { @MainActor in unlocked = true } }
    }
  }
}
