// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassSync
import Foundation

/// The owner's offer to update a server Compass set up on Cloudflare
/// (docs/design/sync.md, "Updating a server").
struct ServerUpdate: Equatable {
  /// The version the server reports (1 for one from before versions).
  var current: Int
  /// The version Compass ships.
  var latest: Int

  /// What the versions the server lacks add, as the desktop's page says.
  var note: String {
    let newIn = [
      2: String(localized: "sharing spaces, folders and easels with others", comment: "Server update: a feature it adds, completes “It adds …”"),
      3: String(
        localized: "notifications on phones when a tab is sent to them, through Compass's push relay at push.compassbrowser.org",
        comment: "Server update: a feature it adds, completes “It adds …”"),
    ]
    let added = stride(from: current + 1, through: latest, by: 1).compactMap { newIn[$0] }
    let adds = added.joined(separator: ", ")
    return (added.isEmpty ? "" : String(localized: "It adds \(adds).", comment: "Server update: what the new version adds") + " ")
      + String(localized: "Your data and the people on it stay as they are.", comment: "Server update: note")
  }
}

/// A server of the user's own on Cloudflare, and the people on it
/// (docs/design/ios.md, "A server on Cloudflare"): the desktop's
/// CompassSyncService StartOnCloudflare(), JoinWithInvite(),
/// GetServerUpdate()/UpdateServer() and the people methods.
extension SyncController {
  /// Where the server Compass set up is (account, script, subdomain; never
  /// the token), as the desktop's `compass.sync.cloudflare`.
  static let cloudflareKey = "CompassSyncCloudflare"

  /// The sync server Compass sets up (server/cloudflare/dist, bundled).
  static var workerScript: String? {
    Bundle.main.url(forResource: "compass-sync", withExtension: "js")
      .flatMap { try? String(contentsOf: $0, encoding: .utf8) }
  }

  /// Its version (src/version.ts); 0 if the build has none.
  static var workerVersion: Int {
    Bundle.main.url(forResource: "compass-sync", withExtension: "version")
      .flatMap { try? String(contentsOf: $0, encoding: .utf8) }
      .flatMap { Int($0.trimmingCharacters(in: .whitespacesAndNewlines)) } ?? 0
  }

  /// A progress line for Settings while `serverSetup` is set (a late one
  /// from a finished run is dropped).
  private func progress() -> CloudflareDeployer.Progress {
    { line in Task { @MainActor [weak self] in if self?.serverSetup != nil { self?.serverSetup = line } } }
  }

  private func record(_ deployment: CloudflareDeployment) {
    UserDefaults.standard.set(
      ["account_id": deployment.accountID, "script": deployment.scriptName, "subdomain": deployment.subdomain],
      forKey: Self.cloudflareKey)
  }

  /// Sets up a server in the user's Cloudflare account with `token` (from
  /// Sign in with Cloudflare, or an API token) and starts a new chain on
  /// it. The chain comes first: the server is deployed with it as its
  /// owner, so from its first request it serves only this chain, its
  /// profiles and the friends it invites. On success `newChainWords` holds
  /// the 12 words; otherwise the reason. Nothing is saved if it fails.
  func startOnCloudflare(token: String, pushRelay: Bool) async -> String? {
    guard chain == nil else { return String(localized: "Sync is already set up.", comment: "Cloudflare server setup or invite: error") }
    let token = token.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !token.isEmpty else { return String(localized: "Paste your Cloudflare token.", comment: "Cloudflare server setup or invite: error") }
    guard serverSetup == nil else { return String(localized: "Sync is being set up.", comment: "Cloudflare server setup or invite: error") }
    guard let script = Self.workerScript, !script.isEmpty else { return String(localized: "This build has no sync server to set up.", comment: "Cloudflare server setup or invite: error") }
    let chain = try! Chain(seed: Chain.generateSeed())
    serverSetup = String(localized: "Connecting to Cloudflare…", comment: "Cloudflare setup: progress")
    defer { serverSetup = nil }
    let deployment: CloudflareDeployment
    do {
      deployment = try await CloudflareDeployer(token: token, script: script)
        .deploy(owner: chain.chainID, pushRelay: pushRelay, progress: progress())
    } catch {
      return error.message
    }
    guard self.chain == nil, let server = deployment.serverURL else { return String(localized: "Sync is already set up.", comment: "Cloudflare server setup or invite: error") }
    record(deployment)
    save(chain: chain, server: server)
    start(chain: chain, server: server)
    newChainWords = chain.recoveryWords
    return nil
  }

  /// Joins a friend's server with their invite link
  /// (`https://<server>/invite#<code>`): a new chain joins the server's
  /// list with the code first, so its first sync request is let in, then
  /// starts as Start Sync does.
  func joinWithInvite(_ text: String) async -> String? {
    guard chain == nil else { return String(localized: "Sync is already set up.", comment: "Cloudflare server setup or invite: error") }
    guard let link = ServerInviteLink(text) else {
      return String(localized: "Paste the whole invite link, like https://compass-sync.example.workers.dev/invite#….", comment: "Cloudflare server setup or invite: error")
    }
    guard serverSetup == nil else { return String(localized: "Sync is being set up.", comment: "Cloudflare server setup or invite: error") }
    let chain = try! Chain(seed: Chain.generateSeed())
    serverSetup = String(localized: "Joining…", comment: "Joining a server with an invite: progress")
    defer { serverSetup = nil }
    do {
      try await AccessClient(serverURL: link.serverURL).join(token: chain.accessToken(), code: link.code)
    } catch {
      return error.message
    }
    guard self.chain == nil else { return String(localized: "Sync is already set up.", comment: "Cloudflare server setup or invite: error") }
    save(chain: chain, server: link.serverURL)
    start(chain: chain, server: link.serverURL)
    newChainWords = chain.recoveryWords
    return nil
  }

  // MARK: People on the server

  /// The access client for the server this chain syncs on, and the home
  /// account's token; nil when sync isn't running.
  private func accessClient() -> (AccessClient, String)? {
    guard let chain, let server = serverURL, state != .removed else { return nil }
    return (AccessClient(serverURL: server), chain.accessToken())
  }

  /// Loads the people on the server (only its owners get them) and, if
  /// there are, whether it has an update. A server without a list, a
  /// friend's chain: nothing to show. No answer keeps what was shown.
  func refreshServerPeople() async {
    guard let (client, token) = accessClient() else {
      serverPeople = nil
      serverUpdate = nil
      return
    }
    do {
      let people = try await client.list(token: token)
      guard client.serverURL == serverURL else { return }
      serverPeople = people
    } catch .failed {
      return
    } catch {
      serverPeople = nil
      serverUpdate = nil
      return
    }
    await refreshServerUpdate()
  }

  /// The update offer: only for a server at the address Compass gives the
  /// ones it sets up, whose /info is a Cloudflare server's (or has no
  /// `server`, a version 1 one), older than the one Compass ships.
  func refreshServerUpdate() async {
    let latest = Self.workerVersion
    guard let (client, _) = accessClient(), latest > 0, CloudflareServerName(serverURL: client.serverURL) != nil,
      let info = await client.info(), info.kind.isEmpty || info.kind == "cloudflare"
    else {
      serverUpdate = nil
      return
    }
    let current = info.kind.isEmpty ? 1 : info.version
    serverUpdate = current < latest ? ServerUpdate(current: current, latest: latest) : nil
  }

  /// Makes a single-use invite link (7 days) named `name` (only the owner
  /// sees it).
  func inviteFriend(name: String) async throws(AccessClient.Failure) -> String {
    guard let (client, token) = accessClient() else { throw .failed }
    let link = try await client.createInvite(token: token, name: name.trimmingCharacters(in: .whitespacesAndNewlines))
    await refreshServerPeople()
    return link
  }

  /// Removes a friend (their chain) or revokes an invite (its code).
  func removeFromServer(_ target: String) async throws(AccessClient.Failure) {
    guard let (client, token) = accessClient(), !target.isEmpty else { throw .failed }
    defer { Task { await refreshServerPeople() } }
    try await client.remove(token: token, target: target)
  }

  /// Phone notifications through the Compass push relay, for the whole
  /// server (docs/design/sync.md, "Push on a Cloudflare server").
  func setServerPushRelay(_ on: Bool) async throws(AccessClient.Failure) {
    guard let (client, token) = accessClient() else { throw .failed }
    serverPeople?.pushRelay = on
    defer { Task { await refreshServerPeople() } }
    try await client.setPushRelay(token: token, on: on)
  }

  /// Uploads the server Compass ships to the owner's Cloudflare account
  /// (`token` from Sign in with Cloudflare, or an API token), keeping its
  /// variables; done once it reports the new version. nil, or the reason.
  func updateServer(token: String) async -> String? {
    guard chain != nil, let server = serverURL, state != .removed else { return String(localized: "Sync isn't running on this device.", comment: "Cloudflare server setup or invite: error") }
    let token = token.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !token.isEmpty else { return String(localized: "Paste your Cloudflare token.", comment: "Cloudflare server setup or invite: error") }
    guard serverSetup == nil else { return String(localized: "The server is being set up or updated.", comment: "Cloudflare server setup or invite: error") }
    let latest = Self.workerVersion
    guard let name = CloudflareServerName(serverURL: server), let script = Self.workerScript, !script.isEmpty,
      latest > 0
    else { return String(localized: "This server can't be updated from here.", comment: "Cloudflare server setup or invite: error") }
    serverSetup = String(localized: "Connecting to Cloudflare…", comment: "Cloudflare setup: progress")
    defer { serverSetup = nil }
    do {
      let deployment = try await CloudflareDeployer(token: token, script: script)
        .update(scriptName: name.script, subdomain: name.subdomain, version: latest, progress: progress())
      // Where it is, for a device that didn't set it up.
      record(deployment)
    } catch {
      return error.message
    }
    serverUpdate = nil
    await refreshServerPeople()
    return nil
  }
}
