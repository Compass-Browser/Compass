// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassSync
import SwiftUI
import WebKit

// A server of the user's own on Cloudflare, joining a friend's with an
// invite, and the people on it (docs/design/ios.md, "A server on
// Cloudflare"). The words are the desktop's sync page's
// (resources/sidebar/settings/sync_page.html).

/// The relay's words, for setup and the owner's switch.
private func pushRelayDetails(host: String) -> String {
  String(
    localized: "When a tab is sent to an iPhone or Android phone, Compass's push relay (\(host)) tells it at once. It gets each phone's notification token, never your tabs or data. Off, sent tabs arrive when the phone next syncs.",
    comment: "Cloudflare server: the Phone Notifications switch's explanation; the host is the relay's")
}

/// What's under way (`SyncController.serverSetup`), or the last error.
private struct SetupStatus: View {
  let step: String?
  let error: String?

  var body: some View {
    if let step {
      HStack(spacing: 10) {
        ProgressView()
        Text(step).font(.footnote).foregroundStyle(.secondary)
      }
      .accessibilityIdentifier("CloudflareProgress")
    } else if let error {
      Text(error).font(.footnote).foregroundStyle(.red)
        .accessibilityIdentifier("CloudflareError")
    }
  }
}

/// "No Server? Set Up a Free One on Cloudflare": Sign in with Cloudflare
/// (or an API token), then `SyncController.startOnCloudflare`. Done, it
/// goes back to Settings, which shows the new chain's 12 words.
struct CloudflareSetupView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State private var pushRelay = true
  @State private var useToken = false
  @State private var token = ""
  @State private var signIn: CloudflareOAuth.Request?
  @State private var error: String?

  private var working: Bool { app.sync.serverSetup != nil }

  var body: some View {
    Form {
      Section {
        Text("Compass sets up a sync server in your own Cloudflare account. It's free, and only your devices, and friends you invite, can use it.")
          .font(.callout)
      }
      Section {
        Toggle(isOn: $pushRelay) {
          VStack(alignment: .leading, spacing: 2) {
            Text("Phone Notifications")
            Text(pushRelayDetails(host: "push.compassbrowser.org") + " " + String(localized: "You can change this later.", comment: "Cloudflare server setup: after the Phone Notifications explanation"))
              .font(.caption).foregroundStyle(.secondary)
          }
        }
        .accessibilityIdentifier("CloudflarePushRelay")
      }
      Section {
        Button("Sign in with Cloudflare") {
          error = nil
          signIn = CloudflareOAuth.makeRequest()
        }
        .disabled(working)
        .accessibilityIdentifier("CloudflareSignIn")
        if !useToken {
          Button("Use an API Token Instead") { useToken = true }.font(.footnote)
        }
      } footer: {
        Text("No Cloudflare account? You can make a free one on the sign-in page. Compass uses the access only to set up the server, and doesn't keep it.")
      }
      if useToken {
        Section {
          SecureField("Cloudflare API token", text: $token)
            .textInputAutocapitalization(.never).autocorrectionDisabled()
          Button("Set Up and Start Sync") { start(token) }
            .disabled(working || token.trimmingCharacters(in: .whitespaces).isEmpty)
        } header: {
          Text("API Token")
        } footer: {
          Text("Open [API Tokens](https://dash.cloudflare.com/profile/api-tokens), choose Create Token, and use the template Edit Cloudflare Workers. Paste the token here.")
        }
      }
      if working || error != nil {
        Section { SetupStatus(step: app.sync.serverSetup, error: error) }
      }
    }
    .navigationTitle("Server on Cloudflare")
    .navigationBarTitleDisplayMode(.inline)
    .sheet(item: $signIn) { request in
      CloudflareSignInSheet(request: request) { token, message in
        signIn = nil
        if let token { start(token) } else { error = message }
      }
    }
  }

  private func start(_ token: String) {
    error = nil
    Task {
      if let message = await app.sync.startOnCloudflare(token: token, pushRelay: pushRelay) {
        error = message
      } else {
        dismiss()
      }
    }
  }
}

/// "I Have an Invite": a friend's invite link to their server
/// (`SyncController.joinWithInvite`).
struct InviteJoinView: View {
  @Environment(AppModel.self) private var app
  @Environment(\.dismiss) private var dismiss
  @State private var link = ""
  @State private var error: String?

  var body: some View {
    Form {
      Section {
        TextField("https://compass-sync.example.workers.dev/invite#…", text: $link, axis: .vertical)
          .keyboardType(.URL).textInputAutocapitalization(.never).autocorrectionDisabled()
          .accessibilityIdentifier("InviteLink")
        PasteButton(payloadType: String.self) { strings in
          if let text = strings.first { Task { @MainActor in link = text } }
        }
        .labelStyle(.titleAndIcon)
      } header: {
        Text("Invite Link")
      } footer: {
        Text("A friend who runs a Compass sync server sent it. You get a sync chain of your own on their server; they can't read your data.")
      }
      Section {
        Button("Join and Start Sync") { join() }
          .disabled(app.sync.serverSetup != nil || link.trimmingCharacters(in: .whitespaces).isEmpty)
        if app.sync.serverSetup != nil || error != nil {
          SetupStatus(step: app.sync.serverSetup, error: error)
        }
      }
    }
    .navigationTitle("Join with an Invite")
    .navigationBarTitleDisplayMode(.inline)
  }

  private func join() {
    error = nil
    Task {
      if let message = await app.sync.joinWithInvite(link) {
        error = message
      } else {
        dismiss()
      }
    }
  }
}

/// People on the server this chain owns: the friends (Remove, behind a
/// confirm) and open invites (Revoke), the server's phone notifications
/// switch, Make an Invite Link (shared with the share sheet), and the
/// update offer.
struct ServerPeopleView: View {
  @Environment(AppModel.self) private var app
  @State private var inviteName = ""
  @State private var inviteLink: URL?
  @State private var copied = false
  @State private var error: String?
  @State private var inviting = false
  @State private var removing: ServerMember?
  @State private var signIn: CloudflareOAuth.Request?
  @State private var useToken = false
  @State private var updateToken = ""
  @State private var updateError: String?

  var body: some View {
    Form {
      if let update = app.sync.serverUpdate { updateSection(update) }
      if let people = app.sync.serverPeople {
        Section {
          ForEach(people.members) { member in
            row(title(member), details(member))
              .swipeActions { Button("Remove") { removing = member }.tint(.red) }
              .contextMenu {
                Button("Remove", systemImage: "person.badge.minus", role: .destructive) { removing = member }
              }
          }
          ForEach(people.invites) { invite in
            row(
              invite.name.isEmpty
                ? String(localized: "Invite", comment: "People on Your Server: an unnamed invite")
                : String(localized: "Invite for \(invite.name)", comment: "People on Your Server: an invite and the friend's name"),
              String(
                localized: "Not used yet · expires \(invite.expires.formatted(date: .abbreviated, time: .omitted))",
                comment: "People on Your Server: an invite's details; the date it expires")
            )
            .swipeActions { Button("Revoke") { remove(invite.code) }.tint(.red) }
            .contextMenu {
              Button("Revoke", systemImage: "xmark.circle", role: .destructive) { remove(invite.code) }
            }
          }
          if people.members.isEmpty && people.invites.isEmpty {
            Text("No one else yet.").font(.footnote).foregroundStyle(.secondary)
          }
        } header: {
          Text("People")
        } footer: {
          Text("Only your devices and the friends you invite can sync on your server. Their data is encrypted on their devices: you can't read it.")
        }
        // A server from before version 3 has no such setting; one with push
        // off for good (no relay) can't turn it on.
        if let on = people.pushRelay, !people.pushRelayHost.isEmpty {
          Section {
            Toggle(
              "Phone Notifications",
              isOn: Binding(get: { on }, set: { new in setPushRelay(new) }))
            .accessibilityIdentifier("ServerPushRelay")
          } footer: {
            Text(pushRelayDetails(host: people.pushRelayHost))
          }
        }
        Section {
          TextField("Their name (only you see it)", text: $inviteName)
          if let inviteLink {
            Text(inviteLink.absoluteString).font(.footnote.monospaced()).textSelection(.enabled)
            Button {
              UIPasteboard.general.url = inviteLink
              copied = true
            } label: {
              Label(copied ? "Copied" : "Copy Invite Link", systemImage: copied ? "checkmark" : "doc.on.doc")
            }
            ShareLink(item: inviteLink, message: Text("Sync with Compass on my server")) {
              Label("Send Invite Link…", systemImage: "square.and.arrow.up")
            }
          }
          Button {
            invite()
          } label: {
            Label(inviteLink == nil ? "Make an Invite Link" : "Make Another Link", systemImage: "link.badge.plus")
          }
          .disabled(inviting)
        } header: {
          Text("Invite a Friend")
        } footer: {
          VStack(alignment: .leading, spacing: 6) {
            if let error { Text(error).foregroundStyle(.red) }
            if inviteLink != nil {
              Text("Send this link to your friend. It works once, within 7 days. In Compass, they choose Sync, then I have an invite.")
            }
          }
        }
      } else {
        Section {
          Text("Only the server's owner can see who's on it.").foregroundStyle(.secondary)
        }
      }
    }
    .navigationTitle("People on Your Server")
    .navigationBarTitleDisplayMode(.inline)
    .task { await app.sync.refreshServerPeople() }
    .refreshable { await app.sync.refreshServerPeople() }
    .onChange(of: inviteName) { if inviteLink != nil { inviteLink = nil } }
    .confirmationDialog(
      "Remove \(removing.map(title) ?? "")?",
      isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } }),
      titleVisibility: .visible, presenting: removing
    ) { member in
      Button("Remove", role: .destructive) { remove(member.chain) }
      Button("Cancel", role: .cancel) {}
    } message: { _ in
      Text("They stop syncing on your server within a minute. Their data stays on their devices. You can invite them again.")
    }
    .sheet(item: $signIn) { request in
      CloudflareSignInSheet(request: request) { token, message in
        signIn = nil
        if let token { update(token) } else { updateError = message }
      }
    }
  }

  @ViewBuilder private func updateSection(_ update: ServerUpdate) -> some View {
    let working = app.sync.serverSetup != nil
    Section {
      VStack(alignment: .leading, spacing: 4) {
        Text("A newer version of your server is ready.").fontWeight(.semibold)
        Text(update.note).font(.footnote).foregroundStyle(.secondary)
      }
      Button("Update with Cloudflare") {
        updateError = nil
        signIn = CloudflareOAuth.makeRequest()
      }
      .disabled(working)
      if useToken {
        SecureField("Cloudflare API token", text: $updateToken)
          .textInputAutocapitalization(.never).autocorrectionDisabled()
        Button("Update") { self.update(updateToken) }
          .disabled(working || updateToken.trimmingCharacters(in: .whitespaces).isEmpty)
      } else {
        Button("Use an API Token Instead") { useToken = true }.font(.footnote)
      }
      if working || updateError != nil {
        SetupStatus(step: app.sync.serverSetup, error: updateError)
      }
    } header: {
      Text("Update")
    }
  }

  private func row(_ title: String, _ details: String) -> some View {
    VStack(alignment: .leading, spacing: 2) {
      Text(title)
      Text(details).font(.caption).foregroundStyle(.secondary)
    }
  }

  private func title(_ member: ServerMember) -> String {
    member.name.isEmpty ? String(localized: "Friend", comment: "People on Your Server: an unnamed friend") : member.name
  }

  /// "Joined Nov 25, 2026 · 2 linked accounts": their other profiles, and
  /// the chains they reset to.
  private func details(_ member: ServerMember) -> String {
    let linked =
      member.linked > 0
      ? " · " + String(localized: "\(member.linked) linked accounts", comment: "People on Your Server: a friend's other profiles and chains")
      : ""
    let joined = String(
      localized: "Joined \(member.added.formatted(date: .abbreviated, time: .omitted))",
      comment: "People on Your Server: when a friend joined")
    return joined + linked
  }

  private func invite() {
    error = nil
    inviting = true
    Task {
      defer { inviting = false }
      do throws(AccessClient.Failure) {
        let link = try await app.sync.inviteFriend(name: inviteName)
        inviteLink = URL(string: link)
        copied = false
      } catch {
        self.error = error.message
      }
    }
  }

  private func remove(_ target: String) {
    error = nil
    Task {
      do throws(AccessClient.Failure) {
        try await app.sync.removeFromServer(target)
      } catch {
        self.error = error.message
      }
    }
  }

  private func setPushRelay(_ on: Bool) {
    error = nil
    Task {
      do throws(AccessClient.Failure) {
        try await app.sync.setServerPushRelay(on)
      } catch {
        self.error = error.message
      }
    }
  }

  private func update(_ token: String) {
    updateError = nil
    Task {
      if let message = await app.sync.updateServer(token: token) {
        updateError = message
      } else {
        updateToken = ""
        useToken = false
      }
    }
  }
}

/// Sign in with Cloudflare in a sheet: Cloudflare's consent page in a web
/// view of its own (nothing kept: a non-persistent data store). The
/// redirect URI is an https URL on compassbrowser.org, which
/// ASWebAuthenticationSession could only catch with an associated domain,
/// so the web view catches the navigation to it instead (it's never
/// loaded), as the desktop's sign-in tab does, and trades the code for a
/// token. `finish` gets the token, or why there's none (nil: closed).
struct CloudflareSignInSheet: View {
  let request: CloudflareOAuth.Request
  let finish: (_ token: String?, _ error: String?) -> Void
  @State private var exchanging = false
  @State private var loadError: String?
  @State private var done = false

  var body: some View {
    NavigationStack {
      CloudflareWebView(url: request.url, onRedirect: redirected, onError: { loadError = $0 })
        .ignoresSafeArea(edges: .bottom)
        .overlay {
          if exchanging {
            ProgressView("Signing in…").padding().background(.regularMaterial, in: .rect(cornerRadius: 12))
          } else if let loadError {
            ContentUnavailableView("Couldn't open Cloudflare's sign-in page.", systemImage: "wifi.exclamationmark",
              description: Text(loadError))
          }
        }
        .navigationTitle("Sign in with Cloudflare")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
          ToolbarItem(placement: .cancellationAction) {
            Button("Cancel") { end(nil, nil) }
          }
        }
    }
    .interactiveDismissDisabled(exchanging)
  }

  private func end(_ token: String?, _ error: String?) {
    guard !done else { return }
    done = true
    finish(token, error)
  }

  /// The desktop's CloudflareSignIn's words.
  private func redirected(_ url: URL) {
    let reply = CloudflareOAuth.parseRedirect(url, expectedState: request.state)
    guard let code = reply.code else {
      end(
        nil,
        reply.error == "access_denied"
          ? String(localized: "You didn't allow Compass on Cloudflare.", comment: "Sign in with Cloudflare: error")
          : String(localized: "Cloudflare sign-in didn't finish. Try again.", comment: "Sign in with Cloudflare: error"))
      return
    }
    exchanging = true
    Task {
      let token = await CloudflareOAuth.exchange(code: code, verifier: request.verifier)
      exchanging = false
      end(token, token == nil ? String(localized: "Cloudflare didn't give Compass access. Try again.", comment: "Sign in with Cloudflare: error") : nil)
    }
  }
}

/// The sign-in page. Pages that open a new window (Cloudflare's terms, a
/// sign-up) load in the same view.
private struct CloudflareWebView: UIViewRepresentable {
  let url: URL
  let onRedirect: (URL) -> Void
  /// Why the page didn't load; nil once one did.
  let onError: (String?) -> Void

  func makeCoordinator() -> Coordinator { Coordinator(self) }

  func makeUIView(context: Context) -> WKWebView {
    let configuration = WKWebViewConfiguration()
    configuration.websiteDataStore = .nonPersistent()
    let view = WKWebView(frame: .zero, configuration: configuration)
    view.navigationDelegate = context.coordinator
    view.uiDelegate = context.coordinator
    view.load(URLRequest(url: url))
    return view
  }

  func updateUIView(_ view: WKWebView, context: Context) {
    context.coordinator.parent = self
  }

  @MainActor final class Coordinator: NSObject, WKNavigationDelegate, WKUIDelegate {
    var parent: CloudflareWebView
    private var redirected = false

    init(_ parent: CloudflareWebView) { self.parent = parent }

    /// Cloudflare answers the consent with a redirect to the redirect URI:
    /// it's caught here (server redirects come through too) and never
    /// loaded.
    nonisolated func webView(
      _ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
      decisionHandler: @escaping @MainActor (WKNavigationActionPolicy) -> Void
    ) {
      MainActor.assumeIsolated {
        guard let url = action.request.url, CloudflareOAuth.isRedirect(url) else {
          return decisionHandler(.allow)
        }
        decisionHandler(.cancel)
        guard !redirected else { return }
        redirected = true
        parent.onRedirect(url)
      }
    }

    nonisolated func webView(
      _ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error
    ) {
      MainActor.assumeIsolated { failed(error) }
    }

    nonisolated func webView(_ webView: WKWebView, didCommit navigation: WKNavigation!) {
      MainActor.assumeIsolated { parent.onError(nil) }
    }

    nonisolated func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
      MainActor.assumeIsolated { failed(error) }
    }

    private func failed(_ error: Error) {
      let error = error as NSError
      // A navigation this view canceled (the redirect), or one replaced.
      if redirected || error.code == NSURLErrorCancelled
        || (error.domain == "WebKitErrorDomain" && error.code == 102)
      {
        return
      }
      parent.onError(error.localizedDescription)
    }

    nonisolated func webView(
      _ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
      for navigationAction: WKNavigationAction, windowFeatures: WKWindowFeatures
    ) -> WKWebView? {
      MainActor.assumeIsolated {
        if navigationAction.targetFrame == nil { webView.load(navigationAction.request) }
        return nil
      }
    }
  }
}
