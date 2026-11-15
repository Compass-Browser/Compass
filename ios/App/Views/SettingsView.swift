// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassBoosts
import CompassCrypto
import CompassModel
import CompassShared
import CompassSync
import LocalAuthentication
import SwiftUI

struct SettingsView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State private var showingRecovery = false
  @State private var confirmLeave = false
  @AppStorage(SearchEngine.key) private var searchEngine = SearchEngine.google.rawValue
  @AppStorage(Spotlight.enabledKey) private var spotlight = true
  @AppStorage(AutoPiP.enabledKey) private var autoPiP = true
  @AppStorage(AutoPiP.scrollKey) private var autoPiPScroll = true
  @AppStorage(StartScreen.key) private var startScreen = StartScreen.sidebar
  @AppStorage(LittleWindows.key) private var littleWindows = true
  // In the App Group: the share extension reads it.
  @AppStorage(SharedContainer.shareOpensLittleWindowKey, store: SharedContainer.defaults)
  private var shareOpensLittleWindow = false

  var body: some View {
    NavigationStack {
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
              SettingsLabel("Claude", "sparkles", .orange)
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
        LinkRulesSection()
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
        if app.sync.isOn { syncOn } else { syncOff }
        Section {
          Link(destination: URL(string: "https://compassbrowser.org/privacy")!) {
            SettingsLabel("Privacy Policy", "hand.raised.fill", .indigo)
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
      .sheet(isPresented: Bindable(app).showingJoinSummary) { JoinSummaryView() }

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
    if !app.sync.profiles.isEmpty {
      Section {
        ForEach(app.sync.profiles) { profile in
          LabeledContent {
            let count = app.sync.spaceCount(profile)
            Text(count == 1 ? "1 space" : "\(count) spaces")
          } label: {
            Text(profile.name.isEmpty ? "Profile" : profile.name)
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
  @State private var server = SyncController.defaultServer?.absoluteString ?? ""
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

/// After joining: what came over. The phone has nothing to configure (M8):
/// every profile of the chain syncs, each in its own account.
struct JoinSummaryView: View {
  @Environment(AppModel.self) private var app

  var body: some View {
    VStack(spacing: 18) {
      Spacer()
      if app.sync.state == .syncing || app.sync.lastSync == nil, !isFailed {
        ProgressView()
        Text("Syncing…").font(.title3.weight(.semibold))
      } else {
        Image(systemName: isFailed ? "exclamationmark.icloud" : "checkmark.icloud")
          .font(.system(size: 44)).foregroundStyle(isFailed ? Color.orange : Color.accentColor)
      }
      Text(summary).font(.body).multilineTextAlignment(.center).padding(.horizontal)
      if case .failed(let message) = app.sync.state {
        Text(message).font(.footnote).foregroundStyle(.secondary).multilineTextAlignment(.center).padding(.horizontal)
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
  @State private var editing: LinkRuleDraft?

  private var rules: [LinkRule] {
    app.store.linkRules.sorted {
      (LinkRouting.displayDomain($0.domain), $0.profile) < (LinkRouting.displayDomain($1.domain), $1.profile)
    }
  }

  var body: some View {
    Section {
      ForEach(rules) { rule in
        Button {
          editing = LinkRuleDraft(
            original: rule.id, domain: LinkRouting.displayDomain(rule.domain),
            spaceID: app.store.spaces.contains { $0.id == rule.spaceID } ? rule.spaceID : defaultSpaceID)
        } label: {
          LinkRuleRow(rule: rule, space: app.store.spaces.first { $0.id == rule.spaceID })
        }
        .foregroundStyle(.primary)
        .swipeActions {
          Button("Delete", role: .destructive) { app.store.removeLinkRule(rule.domain, profile: rule.profile) }
        }
      }
      Button {
        editing = LinkRuleDraft(original: nil, domain: "", spaceID: defaultSpaceID)
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
    .sheet(item: $editing) { draft in LinkRuleEditor(draft: draft) }
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
