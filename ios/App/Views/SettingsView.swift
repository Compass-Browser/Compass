// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import CompassCrypto
import CompassModel
import CompassShared
import CompassSync
import CoreImage.CIFilterBuiltins
import LocalAuthentication
import SwiftUI

struct SettingsView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  /// The sheet Settings shows (SettingsSheet).
  @State private var presented: SettingsSheet?
  @State private var confirmLeave = false
  /// The device "Remove" asks about.
  @State private var removing: SyncDevice?
  /// Why removing a device failed.
  @State private var removeError: String?
  /// The server "Start Sync" uses (the join form's default).
  @State private var startServer = SyncController.defaultServer?.absoluteString ?? ""
  @State private var startError: String?
  @AppStorage(SearchEngine.key) private var searchEngine = SearchEngine.google.rawValue
  @AppStorage(Spotlight.enabledKey) private var spotlight = true
  @AppStorage(AutoPiP.enabledKey) private var autoPiP = true
  @AppStorage(AutoPiP.scrollKey) private var autoPiPScroll = true
  @AppStorage(StartScreen.key) private var startScreen = StartScreen.sidebar
  @AppStorage(LittleWindows.key) private var littleWindows = true
  @AppStorage(CleanLink.removeTrackingKey) private var removeTracking = true
  // In the App Group: the share extension reads it.
  @AppStorage(SharedContainer.shareOpensLittleWindowKey, store: SharedContainer.defaults)
  private var shareOpensLittleWindow = false

  var body: some View {
    NavigationStack {
      ScrollViewReader { scroller in
      Form {
        Section {
          NavigationLink { SiteSettingsView(site: nil) } label: {
            SettingsLabel("Global Site Settings", "globe", .black)
          }
        }
        Section {
          NavigationLink {
            SearchEnginePicker(selection: $searchEngine)
          } label: {
            HStack {
              SettingsLabel("Search Engine", "magnifyingglass", .blue)
              Spacer()
              Text(SearchEngine(rawValue: searchEngine)?.name ?? "").foregroundStyle(.secondary)
            }
          }
          NavigationLink { PasswordsView() } label: {
            SettingsLabel("Passwords", "key.fill", .gray)
          }
          NavigationLink { ClaudeSettingsView() } label: {
            HStack {
              SettingsLabel("AI", "sparkles", .orange)
              Spacer()
              Text(ClaudeSettings.shared.hasKey ? "On" : "Off").foregroundStyle(.secondary)
            }
          }
          NavigationLink { BoostsView() } label: {
            HStack {
              SettingsLabel("Boosts", "wand.and.stars", .pink)
              Spacer()
              if !BoostStore.shared.boosts.isEmpty {
                Text("\(BoostStore.shared.boosts.filter(\.enabled).count)").foregroundStyle(.secondary)
              }
            }
          }
          .accessibilityIdentifier("SettingsBoosts")
          if #available(iOS 18.4, *) {
            NavigationLink { ExtensionsView() } label: {
              HStack {
                SettingsLabel("Extensions", "puzzlepiece.extension.fill", .green)
                Spacer()
                if !app.extensions.records.isEmpty {
                  Text("\(app.extensions.records.filter(\.enabled).count)").foregroundStyle(.secondary)
                }
              }
            }
          }
        }
        Section {
          Picker(selection: $startScreen) {
            ForEach(StartScreen.allCases) { Text($0.name).tag($0) }
          } label: {
            SettingsLabel("On Launch", "power", .orange)
          }
          NavigationLink { ArchiveAfterPicker() } label: {
            HStack {
              SettingsLabel("Archive Inactive Tabs", "archivebox.fill", .purple)
              Spacer()
              Text(ArchiveAfter.current(app.store.spaces)?.name ?? "Mixed").foregroundStyle(.secondary)
            }
          }
          NavigationLink { ClearDataView() } label: {
            SettingsLabel("Clear Browsing Data", "eraser.fill", .red)
          }
          Toggle(isOn: $removeTracking) {
            VStack(alignment: .leading, spacing: 2) {
              SettingsLabel("Remove Tracking from Copied Links", "link", .green)
              Text("Copy Link leaves out parameters that only track where a link was shared, using Firefox's list.")
                .font(.caption).foregroundStyle(.secondary).padding(.leading, 44)
            }
          }
          ShareLink(item: app.openTabsText, subject: Text("Open tabs")) {
            VStack(alignment: .leading, spacing: 2) {
              SettingsLabel("Export Open Tabs", "square.and.arrow.up", .blue)
              Text("Share your open tab URLs").font(.caption).foregroundStyle(.secondary).padding(.leading, 44)
            }
          }
          // Like the other rows, not the accent color.
          .tint(.primary)
        }
        Section {
          Toggle(isOn: $autoPiP) {
            VStack(alignment: .leading, spacing: 2) {
              SettingsLabel("Auto Picture in Picture", "pip.fill", .teal)
              Text("A video playing with sound keeps playing in a window when you leave its tab")
                .font(.caption).foregroundStyle(.secondary).padding(.leading, 44)
            }
          }
          Toggle(isOn: $autoPiPScroll) {
            VStack(alignment: .leading, spacing: 2) {
              SettingsLabel("When Scrolled Away", "arrow.up.and.down", .teal)
              Text("Also when you scroll it off the screen; scrolling back puts it in the page")
                .font(.caption).foregroundStyle(.secondary).padding(.leading, 44)
            }
          }
          .disabled(!autoPiP)
          Button("Copy Diagnostics") {
            UIPasteboard.general.string = AutoPiP.recent.isEmpty
              ? "No auto picture-in-picture activity since Compass started."
              : AutoPiP.recent.joined(separator: "\n")
          }
        } footer: {
          Text("What auto picture in picture did lately, to paste in a bug report.")
        }
        Section {
          Toggle(isOn: $littleWindows) {
            VStack(alignment: .leading, spacing: 2) {
              SettingsLabel("Open in a Little Window", "macwindow", .indigo)
              Text("Links from other apps open in a card over your tabs; Open in a space makes them a tab")
                .font(.caption).foregroundStyle(.secondary).padding(.leading, 44)
            }
          }
          Toggle(isOn: $shareOpensLittleWindow) {
            VStack(alignment: .leading, spacing: 2) {
              SettingsLabel("Share Sheet Opens a Little Window", "square.and.arrow.up.fill", .indigo)
              Text("Sharing a link to Compass skips choosing a space and opens it in a little window")
                .font(.caption).foregroundStyle(.secondary).padding(.leading, 44)
            }
          }
        } header: {
          Text("Links from Other Apps")
        } footer: {
          Text("Links from other apps reach Compass when it's your default browser (Settings → Apps → Default Browser App).")
        }
        LinkRulesSection { presented = .linkRule($0) }
        Section {
          Toggle(isOn: $spotlight) {
            VStack(alignment: .leading, spacing: 2) {
              SettingsLabel("Show in Spotlight", "magnifyingglass.circle.fill", .gray)
              Text("Favorites and pinned tabs in Home Screen search").font(.caption).foregroundStyle(.secondary)
                .padding(.leading, 44)
            }
          }
          .onChange(of: spotlight) { app.system.writeSnapshot(force: true) }
        } footer: {
          Text("To open pages from Safari in Compass, turn on \"Open in Compass\" in Safari → Extensions.")
        }
        Group { if app.sync.isOn { syncOn } else { syncOff } }.id("sync")
        Section {
          Link(destination: URL(string: "https://compassbrowser.org/privacy")!) {
            SettingsLabel("Privacy Policy", "hand.raised.fill", .indigo)
          }
          NavigationLink { ThirdPartyLicensesView() } label: {
            SettingsLabel("Third-Party Licenses", "doc.text.fill", .gray)
          }
        }
      }
      .navigationTitle("Settings")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar { Button("Done") { dismiss() } }
      // A pairing link from the camera goes straight to joining.
      .navigationDestination(
        isPresented: Binding(get: { app.pendingInvite != nil && !app.sync.isOn }, set: { _ in })
      ) { JoinView() }
      .navigationDestination(for: SettingsRoute.self) { route in
        switch route {
        case .join: JoinView()
        }
      }
      // Every sheet and dialog is here, on the Form, none on a row or a
      // section: the List rebuilds those (a sync cycle changes the sync
      // section; Face ID makes the app inactive and active again, which
      // starts one), and a presentation attached to one closed with it,
      // taking the Settings sheet along.
      .sheet(item: sheet) { sheet in
        switch sheet {
        case .recovery: RecoveryCodeView(words: app.sync.recoveryWords, dismissTitle: "Hide")
        case .newWords(let words):
          RecoveryCodeView(words: words, dismissTitle: "I Saved It").interactiveDismissDisabled()
        case .addDevice: AddDeviceView()
        case .joinSummary: JoinSummaryView()
        case .linkRule(let draft): LinkRuleEditor(draft: draft)
        }
      }
      .confirmationDialog("Leave sync?", isPresented: $confirmLeave) {
        Button("Leave Sync", role: .destructive) { app.sync.leave() }
      } message: {
        Text("Your spaces stay on this \(UIDevice.current.model). Other devices keep syncing.")
      }
      .confirmationDialog(
        "Remove \(removing?.name ?? "")?",
        isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } }),
        titleVisibility: .visible, presenting: removing
      ) { device in
        Button("Remove", role: .destructive) { remove(device) }
        Button("Cancel", role: .cancel) {}
      } message: { _ in
        Text(
          "It stops syncing and disappears from your devices. What's on it stays there, and it can join again with a pairing code. Someone who has your recovery code can still join: to lock everyone out, use Reset sync on a computer."
        )
      }
      .alert(
        "Couldn't Remove the Device",
        isPresented: Binding(get: { removeError != nil }, set: { if !$0 { removeError = nil } }),
        presenting: removeError
      ) { _ in
        Button("OK", role: .cancel) {}
      } message: { Text($0) }
      #if DEBUG
        // With -CompassSettings YES, simulator checks of the sync screens:
        //   -CompassSyncScreen start: "Start Sync" (on -CompassServer's server);
        //   add: "Add a Device"; recovery: the words (no Face ID asked);
        //   auth: the words behind Face ID, as Show Recovery Code;
        //   remove: Remove's confirmation for the first other device.
        .task {
          guard let screen = UserDefaults.standard.string(forKey: "CompassSyncScreen") else { return }
          try? await Task.sleep(for: .milliseconds(800))
          scroller.scrollTo("sync", anchor: .top)
          switch screen {
          case "start" where !app.sync.isOn:
            // -CompassServer <url> instead of the build's server.
            if let server = UserDefaults.standard.string(forKey: "CompassServer") { startServer = server }
            startSync()
          case "add" where app.sync.isOn: app.sync.addDevice()
          case "recovery" where app.sync.isOn: presented = .recovery
          // As the button: Face ID or the passcode first.
          case "auth" where app.sync.isOn: authenticate()
          case "remove" where app.sync.isOn:
            // Once the first cycle brought the devices.
            for _ in 0..<50 where !app.sync.devices.contains(where: { !$0.isThisDevice }) {
              try? await Task.sleep(for: .milliseconds(200))
            }
            removing = app.sync.devices.first { !$0.isThisDevice }
          default: break
          }
        }
      #endif
      }
    }
  }

  /// One sheet at a time: Add a Device and the join summary follow the
  /// sync controller and the app model, the others are Settings' own.
  private var sheet: Binding<SettingsSheet?> {
    Binding(
      get: {
        if app.showingJoinSummary { return .joinSummary }
        if app.sync.pairing.step != .none { return .addDevice }
        return presented
      },
      set: { new in
        guard new == nil else {
          presented = new
          return
        }
        if app.showingJoinSummary {
          app.showingJoinSummary = false
        } else if app.sync.pairing.step != .none {
          app.sync.cancelPairing()
        } else {
          presented = nil
        }
      })
  }

  private func startSync() {
    startError = nil
    switch app.sync.startNewChain(server: startServer) {
    case .success(let words): presented = .newWords(words)
    case .failure(let error): startError = error.message
    }
  }

  private func remove(_ device: SyncDevice) {
    Task {
      do {
        try await app.sync.removeDevice(device)
      } catch let failure as DeviceRemover.Failure {
        removeError = failure.message
      } catch {
        removeError = "\(error)"
      }
    }
  }

  @ViewBuilder private var syncOff: some View {
    Section {
      TextField("https://sync.example.com/v2", text: $startServer)
        .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
        .accessibilityIdentifier("SyncServer")
      Button("Start Sync on This \(UIDevice.current.model)") { startSync() }
        .disabled(startServer.trimmingCharacters(in: .whitespaces).isEmpty)
      // A value: the stack owns the pushed form, not this row.
      NavigationLink("I Have Another Device", value: SettingsRoute.join)
    } header: {
      Text("Sync")
    } footer: {
      VStack(alignment: .leading, spacing: 6) {
        if let startError { Text(startError).foregroundStyle(.red) }
        Text("The address of the Compass sync server you run, or one you trust. Every device of a chain uses the same one. Your data is encrypted on your devices; the server can't read it.")
      }
    }
  }

  @ViewBuilder private var syncOn: some View {
    Section {
      LabeledContent("Status", value: status)
      if let server = app.sync.serverURL { LabeledContent("Server", value: server.host() ?? server.absoluteString) }
      if let user = app.sync.signedInUser {
        LabeledContent("Signed in as", value: SyncSignIn.describe(user))
      }
      if let last = app.sync.lastSync {
        LabeledContent("Last synced") { Text(last, style: .relative) }
      }
      if isRemoved {
        Button("Leave Sync", role: .destructive) { confirmLeave = true }
      } else {
        Button("Sync Now") { Task { await app.sync.syncNow() } }
      }
    } header: {
      Text("Sync")
    } footer: {
      if isRemoved { Text(SyncController.removedMessage) }
    }
    if app.sync.state == .loginRequired || app.sync.isSigningIn {
      Section { SyncSignIn() } header: { Text("Sign In") }
    }
    if !app.sync.profiles.isEmpty {
      Section {
        ForEach(app.sync.profiles) { profile in
          LabeledContent {
            let count = app.sync.spaceCount(profile)
            Text(count == 1 ? "1 space" : "\(count) spaces")
          } label: {
            Text(profile.name.isEmpty ? "Profile" : profile.name)
            // A profile account that didn't sync (one the Mac hasn't
            // started yet): the others sync on.
            if let problem = app.sync.problem(profile) {
              Text(problem).font(.caption).foregroundStyle(.secondary)
            }
          }
        }
      } header: {
        Text("Profiles")
      } footer: {
        Text("Each profile of your Mac syncs in its own encrypted account, with its own spaces, passwords and history.")
      }
    }
    Section("Devices") {
      ForEach(app.sync.devices) { device in
        HStack {
          Image(systemName: device.formFactor == .phone ? "iphone" : device.formFactor == .tablet ? "ipad" : "laptopcomputer")
          Text(device.name)
          if device.isThisDevice {
            Text("This \(UIDevice.current.model)").font(.caption).foregroundStyle(.secondary)
          }
          Spacer()
          Text(device.lastUpdated, style: .date).font(.caption).foregroundStyle(.secondary)
        }
        // Every device but this one (that's Leave Sync), as on the desktop.
        .swipeActions {
          if !device.isThisDevice && !isRemoved {
            Button("Remove") { removing = device }.tint(.red)
          }
        }
        .contextMenu {
          if !device.isThisDevice && !isRemoved {
            Button("Remove", systemImage: "trash", role: .destructive) { removing = device }
          }
        }
        .accessibilityIdentifier("SyncDevice")
      }
      if app.sync.devices.isEmpty {
        Text("The devices show once this one has synced.").font(.footnote).foregroundStyle(.secondary)
      }
      // As the desktop's: only once sync runs (not before the first cycle
      // went through, nor while it fails).
      Button("Add a Device") { app.sync.addDevice() }
        .disabled(!canAddDevice)
    }
    // Sharing with other people (App/Rooms).
    Section("Sharing") { SharingNameRow() }
    Section {
      Button("Show Recovery Code") { authenticate() }
      // Removed: it's at the top, by the reason.
      if !isRemoved { Button("Leave Sync", role: .destructive) { confirmLeave = true } }
    } footer: {
      Text("The 12 words that bring your data back if you lose every device. Compass asks for Face ID or your passcode first.")
    }
  }

  private var isRemoved: Bool { app.sync.state == .removed }

  private var canAddDevice: Bool {
    if case .failed = app.sync.state { return false }
    return app.sync.lastSync != nil && !isRemoved
  }

  private var status: String {
    switch app.sync.state {
    case .off: "Off"
    case .syncing: "Syncing…"
    case .on: "On"
    case .failed(let message): message
    case .removed: "Removed from sync"
    case .loginRequired: "Sign-in needed"
    }
  }

  private func authenticate() {
    let context = LAContext()
    var error: NSError?
    guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else {
      presented = .recovery  // no passcode set: nothing to ask for
      return
    }
    // "Compass is trying to show your sync recovery code", as on the Mac.
    context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: "show your sync recovery code") { ok, _ in
      if ok { Task { @MainActor in presented = .recovery } }
    }
  }
}

/// A screen Settings pushes by value.
enum SettingsRoute: Hashable {
  case join
}

/// What Settings presents, all from the Form (SettingsView's `sheet`).
enum SettingsSheet: Identifiable {
  /// The 12 words, behind Face ID or the passcode.
  case recovery
  /// The 12 words of a chain just started here, shown once.
  case newWords([String])
  case addDevice
  case joinSummary
  /// Adding or editing a link rule.
  case linkRule(LinkRuleDraft)

  var id: String {
    switch self {
    case .recovery: "recovery"
    case .newWords: "newWords"
    case .addDevice: "addDevice"
    case .joinSummary: "joinSummary"
    case .linkRule(let draft): "linkRule-\(draft.id)"
    }
  }
}

/// The 12 words, as the desktop's words panel: once after "Start Sync"
/// ("I Saved It"), or on request behind Face ID or the passcode ("Hide").
/// No Copy: the words don't go to the clipboard, which other devices share.
struct RecoveryCodeView: View {
  @Environment(\.dismiss) private var dismiss
  let words: [String]
  var dismissTitle = "Done"

  var body: some View {
    NavigationStack {
      ScrollView {
        VStack(alignment: .leading, spacing: 20) {
          Text("Save these 12 words somewhere safe, such as a password manager. You only need them if you lose every device that syncs: without them, the synced data is gone.")
          LazyVGrid(columns: [GridItem(), GridItem(), GridItem()], spacing: 12) {
            ForEach(Array(words.enumerated()), id: \.offset) { i, word in
              Text("\(i + 1). \(word)").font(.body.monospaced()).frame(maxWidth: .infinity, alignment: .leading)
                .lineLimit(1).minimumScaleFactor(0.7)
            }
          }
          .padding()
          .background(.quaternary.opacity(0.5), in: RoundedRectangle(cornerRadius: 12))
          .privacySensitive()
          .accessibilityIdentifier("RecoveryWords")
          Text("Anyone with these words can read your synced data.")
            .font(.footnote).foregroundStyle(.secondary)
          Button { dismiss() } label: {
            Text(dismissTitle).frame(maxWidth: .infinity)
          }
          .buttonStyle(.borderedProminent)
          .controlSize(.large)
        }
        .padding()
      }
      .navigationTitle("Your Recovery Code")
      .navigationBarTitleDisplayMode(.inline)
    }
  }
}

/// "Add a Device": the code and its QR, then Allow or Deny with the joining
/// device's name, then how it ended, as the desktop's pairing card
/// (docs/design/sync.md, "Pairing", "Pairing QR code").
struct AddDeviceView: View {
  @Environment(AppModel.self) private var app

  var body: some View {
    let pairing = app.sync.pairing
    NavigationStack {
      ScrollView {
        VStack(spacing: 16) {
          if pairing.step == .working {
            ProgressView().padding(.top, 40)
          }
          if pairing.step == .showingCode, let invite = pairing.invite, let qr = QRCode.image(invite.absoluteString) {
            Image(uiImage: qr).interpolation(.none).resizable().scaledToFit()
              .frame(width: 200, height: 200)
              // Black on white with a quiet zone, whatever the theme.
              .padding(16)
              .background(Color.white, in: RoundedRectangle(cornerRadius: 12))
              .accessibilityLabel("Pairing QR code")
              .accessibilityIdentifier("PairingQR")
            Text("On an iPhone, scan this with the camera, or type the code below in Compass.")
              .font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center)
          }
          if !pairing.code.isEmpty {
            Text(pairing.code)
              .font(.system(size: 40, weight: .regular, design: .monospaced)).tracking(3)
              .textSelection(.enabled)
              .accessibilityIdentifier("PairingCode")
          }
          if pairing.step == .showingCode, let expiresAt = pairing.expiresAt {
            TimelineView(.periodic(from: .now, by: 1)) { context in
              let left = max(0, Int(expiresAt.timeIntervalSince(context.date).rounded(.up)))
              Text(left > 0 ? String(format: "Expires in %d:%02d", left / 60, left % 60) : "Expired")
                .font(.footnote).monospacedDigit()
                .foregroundStyle(left <= 30 ? Color.red : Color.secondary)
            }
          }
          if !pairing.message.isEmpty {
            Text(pairing.message)
              .foregroundStyle(pairing.step == .failed ? Color.red : Color.primary)
              .multilineTextAlignment(.center)
          }
          if pairing.step == .confirm {
            Text("Add **\(pairing.deviceName)** to sync?").font(.title3).multilineTextAlignment(.center)
            HStack(spacing: 12) {
              Button { app.sync.answerPairing(allow: false) } label: {
                Text("Deny").frame(maxWidth: .infinity)
              }
              .buttonStyle(.bordered)
              Button { app.sync.answerPairing(allow: true) } label: {
                Text("Allow").frame(maxWidth: .infinity)
              }
              .buttonStyle(.borderedProminent)
            }
            .controlSize(.large)
          }
        }
        .padding()
        .frame(maxWidth: .infinity)
      }
      .navigationTitle("Add a Device")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .cancellationAction) {
          Button(pairing.step == .done || pairing.step == .failed ? "Close" : "Cancel") {
            app.sync.cancelPairing()
          }
        }
      }
    }
    .interactiveDismissDisabled(pairing.step == .confirm)
    #if DEBUG
      // -CompassPairingAnswer allow|deny: answers 4 s after the request
      // shows (simulator checks).
      .task(id: pairing.step) {
        guard pairing.step == .confirm,
          let answer = UserDefaults.standard.string(forKey: "CompassPairingAnswer")
        else { return }
        try? await Task.sleep(for: .seconds(4))
        if !Task.isCancelled { app.sync.answerPairing(allow: answer == "allow") }
      }
    #endif
  }
}

/// A QR code's modules as an image, one pixel each (CoreImage's generator,
/// which adds a one-module margin).
enum QRCode {
  static func image(_ text: String) -> UIImage? {
    let filter = CIFilter.qrCodeGenerator()
    filter.message = Data(text.utf8)
    filter.correctionLevel = "M"
    guard let output = filter.outputImage,
      let cg = CIContext().createCGImage(output, from: output.extent)
    else { return nil }
    return UIImage(cgImage: cg)
  }
}

/// Joining a chain: the code a Mac shows (typed, or scanned from its QR),
/// or the 12-word recovery code.
struct JoinView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State private var server = SyncController.defaultServer?.absoluteString ?? ""
  @State private var code = ""
  @State private var words = ""
  @State private var useRecovery = false
  @State private var working = false
  @State private var waitingForMac = false
  @State private var message: String?
  @State private var scanning = false
  /// The server's identity provider, when it requires sign-in.
  @State private var loginProvider: String?

  var body: some View {
    Form {
      Section {
        TextField("https://sync.example.com/v2", text: $server)
          .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
      } header: {
        Text("Server")
      } footer: {
        if let loginProvider {
          Text("This server requires sign-in with \(loginProvider.isEmpty ? "your company's account" : loginProvider). A chain your Mac already signed in syncs at once; otherwise you'll sign in after joining.")
        }
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
          Text("The code shown on the other device")
        } footer: {
          Text("On a device that syncs, open Sync and choose Add a Device.")
        }
      }
      Section {
        Button(waitingForMac ? "Confirm on the other device…" : "Join") { join() }
          .disabled(working || URL(string: server) == nil)
        Button(useRecovery ? "Use a code from another device instead" : "Use a recovery code") { useRecovery.toggle() }
          .font(.footnote)
      } footer: {
        if let message { Text(message).foregroundStyle(.red) }
      }
    }
    .navigationTitle("Join Sync")
    // Does the server require company sign-in? Asked as the address settles.
    .task(id: server) {
      loginProvider = nil
      guard let url = URL(string: server.trimmingCharacters(in: .whitespaces)), url.host() != nil else { return }
      try? await Task.sleep(for: .milliseconds(600))
      guard !Task.isCancelled else { return }
      if let name = try? await LoginClient.info(serverURL: url), !Task.isCancelled { loginProvider = name }
    }
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
        app.showingJoinSummary = true
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
      case .joined:
        dismiss()
        app.showingJoinSummary = true
      // The desktop's words (JoinResultMessage()).
      case .denied: message = "Not added: the other device said no."
      case .badCode: message = "Enter the code as shown, like 7-482913."
      case .noSuchCode: message = "No such code on this server, or it expired."
      case .codeUsed: message = "That code was already used. Ask for a new one."
      case .wrongCode: message = "Wrong code. Ask the other device for a new one."
      case .canceled: message = "The other device canceled. Ask for a new code."
      case .closed: message = "The other device ended pairing. Check the code and ask for a new one."
      case .expired: message = "Code expired. Ask for a new one."
      case .rateLimited: message = "Too many attempts. Try again in a few minutes."
      case .failed: message = "Couldn't reach the server."
      }
    }
  }
}

/// After joining: what came over. The phone has nothing to configure (M8):
/// every profile of the chain syncs, each in its own account.
struct JoinSummaryView: View {
  @Environment(AppModel.self) private var app

  var body: some View {
    VStack(spacing: 18) {
      Spacer()
      if needsSignIn {
        Image(systemName: "person.crop.circle.badge.exclamationmark")
          .font(.system(size: 44)).foregroundStyle(Color.orange)
      } else if app.sync.state == .syncing || app.sync.lastSync == nil, !isFailed {
        ProgressView()
        Text("Syncing…").font(.title3.weight(.semibold))
      } else {
        Image(systemName: isFailed ? "exclamationmark.icloud" : "checkmark.icloud")
          .font(.system(size: 44)).foregroundStyle(isFailed ? Color.orange : Color.accentColor)
      }
      if !needsSignIn { Text(summary).font(.body).multilineTextAlignment(.center).padding(.horizontal) }
      if case .failed(let message) = app.sync.state {
        Text(message).font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center).padding(.horizontal)
      }
      if needsSignIn {
        SyncSignIn().padding(.horizontal)
      }
      Spacer()
      Button {
        app.showingJoinSummary = false
        app.showingSettings = false
      } label: {
        Text("Continue").frame(maxWidth: .infinity)
      }
      .buttonStyle(.borderedProminent)
      .controlSize(.large)
      .padding()
    }
    .interactiveDismissDisabled()
  }

  private var isFailed: Bool { if case .failed = app.sync.state { true } else { false } }
  private var needsSignIn: Bool { app.sync.state == .loginRequired || app.sync.isSigningIn }

  /// "Synced 2 profiles: Personal, Work · 5 spaces".
  private var summary: String {
    let names = app.sync.profiles.map(\.name).filter { !$0.isEmpty }
    let count = max(app.sync.profiles.count, 1)
    let spaces = app.store.spaces.count
    var text = count == 1 ? "Synced 1 profile" : "Synced \(count) profiles"
    if !names.isEmpty { text += ": " + names.joined(separator: ", ") }
    return text + " · " + (spaces == 1 ? "1 space" : "\(spaces) spaces")
  }
}

/// Company sign-in (docs/design/sync.md, "Company sign-in (OIDC)"): the
/// server requires the chain to be signed in with its identity provider.
/// "Sign in" opens the server's page; while it's open, the code the page
/// must show too, so a link someone else sent can't sign their device in.
struct SyncSignIn: View {
  @Environment(AppModel.self) private var app

  static func describe(_ user: LoginClient.User) -> String {
    switch (user.name.isEmpty, user.email.isEmpty) {
    case (false, false): "\(user.name) (\(user.email))"
    case (true, false): user.email
    default: user.name
    }
  }

  private var provider: String {
    app.sync.loginProvider.flatMap { $0.isEmpty ? nil : $0 } ?? "your company's account"
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 10) {
      if let code = app.sync.loginCode {
        Text("Check that the sign-in page shows this code:").font(.footnote).foregroundStyle(.secondary)
        Text(code).font(.title2.monospaced().weight(.semibold)).textSelection(.enabled)
          .accessibilityLabel("Code \(code.map(String.init).joined(separator: " "))")
        HStack {
          ProgressView()
          Text("Waiting for the sign-in…").font(.footnote).foregroundStyle(.secondary)
          Spacer()
          Button("Cancel", role: .cancel) { app.sync.cancelLogin() }
        }
      } else if app.sync.isSigningIn {
        ProgressView()
      } else {
        Text("This server requires sign-in with \(provider) before it syncs. Your data stays encrypted: signing in doesn't give the server or \(provider) your keys.")
          .font(.footnote).foregroundStyle(.secondary)
        Button("Sign in with \(provider)") { app.sync.startLogin() }
          .buttonStyle(.borderedProminent)
      }
      if let message = app.sync.loginMessage {
        Text(message).font(.footnote).foregroundStyle(.red)
      }
    }
    .padding(.vertical, 4)
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
              Text((p.isBlocked ? "Never saved" : p.username) + profileSuffix(p)).font(.caption).foregroundStyle(.secondary)
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

  /// " · Work" for another profile's password.
  private func profileSuffix(_ p: SavedPassword) -> String {
    guard !p.profile.isEmpty, let name = app.sync.profiles.first(where: { $0.uuid == p.profile })?.name else {
      return ""
    }
    return " · " + name
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

/// A row's label in Settings: a white glyph on a colored square, then the
/// title.
struct SettingsLabel: View {
  let title: String
  let symbol: String
  let color: Color

  init(_ title: String, _ symbol: String, _ color: Color) {
    self.title = title
    self.symbol = symbol
    self.color = color
  }

  var body: some View {
    HStack(spacing: 12) {
      Image(systemName: symbol).menuIcon(color)
      Text(title).foregroundStyle(.primary)
    }
  }
}

/// A wide button on top of Settings (Archive, Downloads).
struct SettingsTile: View {
  let title: String
  let symbol: String
  let action: () -> Void

  var body: some View {
    Button(action: action) { Label(title, systemImage: symbol).tileLabel() }
      .buttonStyle(.plain)
  }
}

extension Label where Title == Text, Icon == Image {
  /// A Settings tile's look: a wide capsule.
  func tileLabel() -> some View {
    self.font(.body.weight(.medium))
      .foregroundStyle(.primary)
      .frame(maxWidth: .infinity)
      .frame(height: 50)
      .background(Color(.secondarySystemGroupedBackground), in: Capsule())
  }
}

struct SearchEnginePicker: View {
  @Binding var selection: String

  var body: some View {
    List {
      ForEach(SearchEngine.allCases) { engine in
        Button {
          selection = engine.rawValue
        } label: {
          HStack {
            Text(engine.name).foregroundStyle(.primary)
            Spacer()
            if selection == engine.rawValue {
              Image(systemName: "checkmark").foregroundStyle(Color.accentColor)
            }
          }
        }
      }
    }
    .navigationTitle("Search Engine")
  }
}

// MARK: Link rules (docs/design/link-routing.md)

/// Settings → Links from Other Apps → Sites: link rules, a domain → a space
/// each, as the desktop's settings section. A rule whose space was deleted
/// says so until it's edited or deleted. They sync with the spaces.
struct LinkRulesSection: View {
  @Environment(AppModel.self) private var app
  /// Opens the editor: Settings presents it (SettingsSheet), not this
  /// section, which the List rebuilds.
  let edit: (LinkRuleDraft) -> Void

  private var rules: [LinkRule] {
    app.store.linkRules.sorted {
      (LinkRouting.displayDomain($0.domain), $0.profile) < (LinkRouting.displayDomain($1.domain), $1.profile)
    }
  }

  var body: some View {
    Section {
      ForEach(rules) { rule in
        Button {
          edit(LinkRuleDraft(
            original: rule.id, domain: LinkRouting.displayDomain(rule.domain),
            spaceID: app.store.spaces.contains { $0.id == rule.spaceID } ? rule.spaceID : defaultSpaceID))
        } label: {
          LinkRuleRow(rule: rule, space: app.store.spaces.first { $0.id == rule.spaceID })
        }
        .foregroundStyle(.primary)
        .swipeActions {
          Button("Delete", role: .destructive) { app.store.removeLinkRule(rule.domain, profile: rule.profile) }
        }
      }
      Button {
        edit(LinkRuleDraft(original: nil, domain: "", spaceID: defaultSpaceID))
      } label: {
        Label("Add Site", systemImage: "plus")
      }
    } header: {
      Text("Sites in Spaces")
    } footer: {
      Text(
        "A link from another app to one of these sites, or to its subdomains, opens as a new tab in that space. The most specific site wins; other links open as above. Never in Incognito."
      )
    }
  }

  private var defaultSpaceID: String {
    let shown = app.selectedSpace.flatMap { $0.isIncognito ? nil : $0 }
    return (shown ?? app.store.spaces.first)?.id ?? ""
  }
}

struct LinkRuleDraft: Identifiable {
  let id = UUID()
  /// The rule being edited; nil for a new one.
  var original: LinkRule.ID?
  var domain: String
  var spaceID: String
}

/// A rule: the site, then its space's icon on its color and its name.
private struct LinkRuleRow: View {
  let rule: LinkRule
  let space: Space?

  var body: some View {
    HStack(spacing: 10) {
      Text(LinkRouting.displayDomain(rule.domain)).lineLimit(1)
      Spacer(minLength: 8)
      if let space {
        HStack(spacing: 6) {
          SpaceIcon(space: space, size: 11)
            .foregroundStyle(.white)
            .frame(width: 20, height: 20)
            .background(Circle().fill(space.theme.accent))
          Text(space.name.isEmpty ? "Space" : space.name).foregroundStyle(.secondary).lineLimit(1)
        }
      } else {
        Label("Space deleted", systemImage: "exclamationmark.triangle.fill")
          .foregroundStyle(.orange)
          .font(.subheadline)
      }
    }
    .accessibilityElement(children: .combine)
  }
}

/// Adding or editing a rule: the site (a domain or a URL) and its space.
private struct LinkRuleEditor: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State var draft: LinkRuleDraft
  @FocusState private var focused: Bool

  private var normalized: String? { LinkRouting.normalizedDomain(draft.domain) }

  var body: some View {
    NavigationStack {
      Form {
        Section {
          TextField("example.com", text: $draft.domain)
            .keyboardType(.URL)
            .textInputAutocapitalization(.never)
            .autocorrectionDisabled()
            .focused($focused)
            .onAppear { focused = draft.original == nil }
        } header: {
          Text("Site")
        } footer: {
          if let normalized {
            Text("Links to \(LinkRouting.displayDomain(normalized)) and its subdomains.")
          } else if !draft.domain.trimmingCharacters(in: .whitespaces).isEmpty {
            Text("Not a domain: type one like example.com, or a link.").foregroundStyle(.red)
          }
        }
        Section("Opens In") {
          Picker("Space", selection: $draft.spaceID) {
            ForEach(app.store.spaces, id: \.id) { space in
              Label {
                Text(space.name.isEmpty ? "Space" : space.name)
              } icon: {
                SpaceIcon(space: space, size: 16).foregroundStyle(space.theme.accent)
              }
              .tag(space.id)
            }
          }
          .pickerStyle(.inline)
          .labelsHidden()
        }
      }
      .navigationTitle(draft.original == nil ? "Add Site" : "Edit Site")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
        ToolbarItem(placement: .confirmationAction) {
          Button("Save", action: save).disabled(normalized == nil || app.store.space(id: draft.spaceID) == nil)
        }
      }
    }
    .presentationDetents([.medium, .large])
  }

  private func save() {
    guard let domain = normalized, let space = app.store.space(id: draft.spaceID) else { return }
    app.store.batch {
      if let original = draft.original,
        original != LinkRule.ID(profile: app.store.linkRuleProfile(space.profileUUID), domain: domain)
      {
        app.store.removeLinkRule(original.domain, profile: original.profile)
      }
      app.store.setLinkRule(domain, to: space)
    }
    dismiss()
  }
}

/// The licenses of what Compass bundles from others: Firefox's
/// strip-on-share lists (third_party/strip_on_share/), with their texts as
/// bundled.
struct ThirdPartyLicensesView: View {
  struct License: Identifiable {
    let title: String
    let path: String
    var id: String { path }
  }

  static let licenses = [
    License(title: "Firefox's strip-on-share list (MPL 2.0)", path: "strip_on_share/MPL2/LICENSE"),
    License(title: "Firefox's strip-on-share list, from ClearURLs (LGPL 3.0)", path: "strip_on_share/LGPL/LICENSE"),
  ]

  var body: some View {
    List(Self.licenses) { license in
      NavigationLink { LicenseTextView(license: license) } label: {
        VStack(alignment: .leading, spacing: 2) {
          Text(license.title)
          Text("Used by Remove Tracking from Copied Links").font(.caption).foregroundStyle(.secondary)
        }
      }
    }
    .navigationTitle("Third-Party Licenses")
    .navigationBarTitleDisplayMode(.inline)
  }
}

private struct LicenseTextView: View {
  let license: ThirdPartyLicensesView.License

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 12) {
        Text(license.title).font(.headline)
        Text(
          "Used by Remove Tracking from Copied Links. Source: https://github.com/mozilla-firefox/firefox, toolkit/components/antitracking/StripOnShareLists."
        )
        .font(.caption).foregroundStyle(.secondary)
        Text(text)
          .font(.system(.caption, design: .monospaced))
          .textSelection(.enabled)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      .padding()
    }
    .navigationTitle("License")
    .navigationBarTitleDisplayMode(.inline)
  }

  private var text: String {
    Bundle.main.resourceURL.flatMap {
      try? String(contentsOf: $0.appendingPathComponent(license.path), encoding: .utf8)
    } ?? "The license text is missing from this build."
  }
}
