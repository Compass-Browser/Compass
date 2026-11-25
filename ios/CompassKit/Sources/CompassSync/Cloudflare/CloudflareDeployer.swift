// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import Foundation

/// What a Compass sync server says of itself: `GET <server>/compass/info`'s
/// `"server"`, `{"kind": "cloudflare", "version": N}` (server/cloudflare,
/// src/version.ts). nil for an answer that isn't JSON; a server without it
/// (the Go server, Cloudflare servers from before versions) gives an empty
/// kind and version 0.
public struct ServerInfo: Sendable, Equatable {
  public var kind: String
  public var version: Int

  public init(kind: String = "", version: Int = 0) {
    self.kind = kind
    self.version = version
  }

  public static func parse(_ data: Data) -> ServerInfo? {
    guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
    var info = ServerInfo()
    if let server = json["server"] as? [String: Any] {
      info.kind = server["kind"] as? String ?? ""
      info.version = (server["version"] as? NSNumber)?.intValue ?? 0
    }
    return info
  }
}

/// The Worker and workers.dev subdomain of a server URL Compass sets up,
/// `https://<script>.<subdomain>.workers.dev/v2`; nil for any other URL.
public struct CloudflareServerName: Sendable, Equatable {
  public var script: String
  public var subdomain: String

  public init?(serverURL url: URL) {
    let suffix = ".workers.dev"
    guard url.scheme?.lowercased() == "https", let host = url.host()?.lowercased(), host.hasSuffix(suffix)
    else { return nil }
    let parts = host.dropLast(suffix.count).split(separator: ".", omittingEmptySubsequences: false)
    guard parts.count == 2, !parts[0].isEmpty, !parts[1].isEmpty else { return nil }
    script = String(parts[0])
    subdomain = String(parts[1])
  }
}

/// Where the deployed server ended up.
public struct CloudflareDeployment: Sendable, Equatable {
  public var accountID = ""
  public var scriptName = ""
  /// The account's workers.dev subdomain.
  public var subdomain = ""
  /// `https://<script>.<subdomain>.workers.dev/v2`, the sync server URL.
  public var serverURL: URL?
}

/// Deploys the Compass sync server to the user's own Cloudflare account
/// (server/cloudflare/README.md; docs/design/sync.md, "A free server on
/// Cloudflare"), through Cloudflare's API with an access token the user gave
/// (an API token, or a "Sign in with Cloudflare" access token). A port of
/// the desktop's components/sync/cloudflare_deployer.cc:
///
/// 1. `GET /accounts`: the account (the first the token can use).
/// 2. `GET /accounts/{id}/workers/subdomain`, or `PUT` one if the account
///    has none ("compass-" and 8 random letters).
/// 3. `GET /accounts/{id}/workers/scripts`: "compass-sync" unless the
///    account has one, then "compass-sync-" and the first 6 hex of the
///    owner chain.
/// 4. `PUT` the script (ES module and metadata: the Durable Object
///    bindings, their SQLite migration as the script's tag needs, and
///    `COMPASS_SYNC_ALLOWED_CHAINS` = the owner chain).
/// 5. `POST` its workers.dev route on.
/// 6. `GET <host>/healthz` until "ok" (a new subdomain's DNS can take
///    minutes).
///
/// `update` instead finds the account whose workers.dev subdomain is the
/// server's, uploads the script keeping its variables, and waits until
/// `/v2/compass/info` reports the new version. The token is used for these
/// requests only, never stored.
public actor CloudflareDeployer {
  /// The Worker script's settings that Compass deploys with
  /// (server/cloudflare/wrangler.jsonc).
  public static let compatibilityDate = "2026-11-01"
  /// The Durable Object migrations, as in wrangler.jsonc: v1 added Chain,
  /// Pairing and Access, v2 Room.
  public static let migrationTag = "v2"
  public static let scriptName = "compass-sync"
  public static let apiBase = URL(string: "https://api.cloudflare.com/client/v4")!

  public struct Failure: Error, Sendable, Equatable {
    public enum Kind: Sendable, Equatable {
      /// 401 or 403 from the API: a wrong token, or one without the
      /// permissions.
      case unauthorized
      /// The token can use no account.
      case noAccount
      /// Any other refusal from the API (its message is kept).
      case api
      /// No answer from the API.
      case network
      /// Deployed, but the server didn't answer in time.
      case notReachable
      /// Update: the signed-in account has no such server.
      case notFound
    }
    public var kind: Kind
    /// The API's own message, for Settings.
    public var detail = ""
    /// The call that failed, its path with the account ID left out.
    public var step = ""
    public var status = 0
    /// Set for `.notReachable`: where it is.
    public var deployment: CloudflareDeployment?

    /// The desktop's CloudflareDeployer::Describe().
    public var message: String {
      switch kind {
      case .unauthorized:
        var s = "Cloudflare didn't accept the token. Check that it's complete and can edit Workers."
        if !detail.isEmpty || !step.isEmpty {
          s += " (" + step + (step.isEmpty ? "" : ": ") + String(status) + (detail.isEmpty ? "" : " " + detail) + ")"
        }
        return s
      case .noAccount: return "The token can't use any Cloudflare account."
      case .api: return detail.isEmpty ? "Cloudflare refused to set up the server." : "Cloudflare refused: " + detail
      case .network: return "Couldn't reach Cloudflare. Check your connection and try again."
      case .notReachable: return "The server is set up but doesn't answer yet. Try again in a few minutes."
      case .notFound:
        return "Your server isn't in the Cloudflare account you signed in with. Sign in with the account it was set up in."
      }
    }
  }

  public typealias Progress = @Sendable (String) -> Void

  private let token: String
  private let script: String
  private let apiBase: URL
  private let session: URLSession
  private let healthTimeout: Duration
  private let healthInterval: Duration

  private var deployment = CloudflareDeployment()
  private var step = ""
  private var status = 0

  /// `script` is the server's bundled ES module (dist/compass-sync.js).
  public init(
    token: String, script: String, apiBase: URL = CloudflareDeployer.apiBase, session: URLSession = .shared,
    healthTimeout: Duration = .seconds(240), healthInterval: Duration = .seconds(3)
  ) {
    self.token = token
    self.script = script
    self.apiBase = apiBase
    self.session = session
    self.healthTimeout = healthTimeout
    self.healthInterval = healthInterval
  }

  // MARK: Deploying

  /// Deploys a server owned by `owner` (the chain's hex public key).
  /// `pushRelay` false: the server starts with phone notifications through
  /// the Compass push relay off (`COMPASS_SYNC_PUSH_DEFAULT=off`); its
  /// owners can turn them on later.
  public func deploy(owner: String, pushRelay: Bool = true, progress: Progress = { _ in }) async throws(Failure)
    -> CloudflareDeployment
  {
    deployment = CloudflareDeployment()
    progress("Checking your Cloudflare account…")
    let accounts = try await self.accounts()
    guard let first = accounts.first else { throw fail(.noAccount) }
    deployment.accountID = first

    let (status, json) = try await call("GET", "/accounts/\(first)/workers/subdomain")
    if status == 401 || status == 403 { throw failFor(status, json) }
    if let name = Self.subdomain(status, json) {
      deployment.subdomain = name
    } else {
      // The account has no workers.dev subdomain yet (404).
      deployment.subdomain = try await createSubdomain(progress: progress)
    }

    let scripts = try await listScripts()
    // An account that has a "compass-sync" already (set up before, by hand
    // or for another chain) keeps it: this one gets a name of its own.
    deployment.scriptName =
      scripts[Self.scriptName] != nil ? Self.scriptName + "-" + owner.prefix(6).lowercased() : Self.scriptName
    progress("Uploading the sync server…")
    try await upload(
      metadata: Self.makeMetadata(owner: owner, migrationTag: scripts[deployment.scriptName] ?? "", pushRelay: pushRelay))

    let (routeStatus, routeJSON) = try await call(
      "POST", "/accounts/\(first)/workers/scripts/\(deployment.scriptName)/subdomain",
      body: try! JSONSerialization.data(withJSONObject: ["enabled": true]), contentType: "application/json")
    guard Self.succeeded(routeStatus, routeJSON) else { throw failFor(routeStatus, routeJSON) }

    let server = serverURL
    deployment.serverURL = server
    progress("Waiting for the server to come online…")
    // Its address resolving and answering; a new workers.dev name can take
    // a few minutes.
    var health = URLComponents(url: server, resolvingAgainstBaseURL: false)!
    health.path = "/healthz"
    try await waitUntil(health.url!) { String(decoding: $0, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines) == "ok" }
    return deployment
  }

  // MARK: Updating

  /// Updates a server Compass set up (docs/design/sync.md, "Updating a
  /// server"): the account whose workers.dev subdomain is `subdomain` and
  /// that has the Worker `scriptName` gets the bundled server, with its
  /// variables kept as they are (keep_bindings: the owners may be another
  /// chain than this device's) and the migration its tag needs. Done once
  /// the server's /v2/compass/info says `version` or later.
  public func update(scriptName: String, subdomain: String, version: Int, progress: Progress = { _ in })
    async throws(Failure) -> CloudflareDeployment
  {
    deployment = CloudflareDeployment(scriptName: scriptName, subdomain: subdomain)
    progress("Finding your server on Cloudflare…")
    var found = false
    for account in try await accounts() {
      deployment.accountID = account
      let (status, json) = try await call("GET", "/accounts/\(account)/workers/subdomain")
      if status == 401 { throw failFor(status, json) }
      // Not this account (or one the sign-in can't read): the next.
      if Self.subdomain(status, json) == subdomain {
        found = true
        break
      }
    }
    guard found else { throw fail(.notFound) }

    let scripts = try await listScripts()
    // Another account with this subdomain can't exist; the script is gone.
    guard let tag = scripts[scriptName] else { throw fail(.notFound) }
    progress("Uploading the new version…")
    try await upload(metadata: Self.makeMetadata(owner: "", migrationTag: tag))

    // Its route is on already.
    let server = serverURL
    deployment.serverURL = server
    progress("Waiting for the new version to answer…")
    try await waitUntil(ServerEndpoint.url(server, path: "compass/info", query: [:])) {
      (ServerInfo.parse($0)?.version ?? 0) >= version
    }
    return deployment
  }

  // MARK: Metadata

  /// The script's metadata part. `migrationTag` is the script's current
  /// one: "" for a new script (all the classes, as v2), "v1" (Room added),
  /// "v2" (none). An empty `owner` (an update) keeps the script's variables
  /// instead of setting the owners (and `pushRelay`, which only a new
  /// server takes).
  public static func makeMetadata(owner: String, migrationTag: String, pushRelay: Bool = true) -> Data {
    var metadata: [String: Any] = ["main_module": "index.js", "compatibility_date": compatibilityDate]
    var bindings: [[String: Any]] = [("CHAIN", "Chain"), ("PAIRING", "Pairing"), ("ACCESS", "Access"), ("ROOM", "Room")]
      .map { ["type": "durable_object_namespace", "name": $0.0, "class_name": $0.1] }
    if owner.isEmpty {
      // An update: the owners and any other variable stay as they are.
      metadata["keep_bindings"] = ["plain_text", "secret_text"]
    } else {
      bindings.append(["type": "plain_text", "name": "COMPASS_SYNC_ALLOWED_CHAINS", "text": owner.lowercased()])
      if !pushRelay {
        bindings.append(["type": "plain_text", "name": "COMPASS_SYNC_PUSH_DEFAULT", "text": "off"])
      }
    }
    metadata["bindings"] = bindings
    // The SQLite migration of the classes the script doesn't have yet;
    // Cloudflare refuses one a script already ran (412).
    if migrationTag.isEmpty {
      metadata["migrations"] = ["new_tag": Self.migrationTag, "new_sqlite_classes": ["Chain", "Pairing", "Access", "Room"]]
    } else if migrationTag == "v1" {
      metadata["migrations"] = ["old_tag": "v1", "new_tag": Self.migrationTag, "new_sqlite_classes": ["Room"]]
    }
    return try! JSONSerialization.data(withJSONObject: metadata, options: [.sortedKeys, .withoutEscapingSlashes])
  }

  // MARK: Steps

  private var serverURL: URL {
    URL(string: "https://\(deployment.scriptName).\(deployment.subdomain).workers.dev/v2")!
  }

  /// The IDs of the accounts the token can use (at least one).
  private func accounts() async throws(Failure) -> [String] {
    let (status, json) = try await call("GET", "/accounts?per_page=50")
    guard Self.succeeded(status, json) else { throw failFor(status, json) }
    let ids = ((json?["result"] as? [Any]) ?? []).compactMap { ($0 as? [String: Any])?["id"] as? String }
      .filter { !$0.isEmpty }
    guard !ids.isEmpty else { throw fail(.noAccount) }
    return ids
  }

  private func createSubdomain(progress: Progress) async throws(Failure) -> String {
    progress("Making your workers.dev address…")
    for attempt in 1...3 {
      let name = "compass-" + Self.randomLetters(8)
      let (status, json) = try await call(
        "PUT", "/accounts/\(deployment.accountID)/workers/subdomain",
        body: try! JSONSerialization.data(withJSONObject: ["subdomain": name]), contentType: "application/json")
      if let made = Self.subdomain(status, json) { return made }
      // Most likely taken: another name.
      if status == 401 || status == 403 || attempt == 3 { throw failFor(status, json) }
    }
    throw fail(.api)
  }

  /// Each script of the account, with its Durable Object migration tag.
  private func listScripts() async throws(Failure) -> [String: String] {
    let (status, json) = try await call("GET", "/accounts/\(deployment.accountID)/workers/scripts")
    guard Self.succeeded(status, json) else { throw failFor(status, json) }
    var scripts: [String: String] = [:]
    for case let script as [String: Any] in (json?["result"] as? [Any]) ?? [] {
      if let id = script["id"] as? String { scripts[id] = script["migration_tag"] as? String ?? "" }
    }
    return scripts
  }

  private func upload(metadata: Data) async throws(Failure) {
    let boundary = "compass-" + Self.randomLetters(24)
    var body = Data()
    func part(_ name: String, _ type: String, _ content: Data) {
      body.append(
        Data(
          "--\(boundary)\r\nContent-Disposition: form-data; name=\"\(name)\"; filename=\"\(name)\"\r\nContent-Type: \(type)\r\n\r\n"
            .utf8))
      body.append(content)
      body.append(Data("\r\n".utf8))
    }
    part("metadata", "application/json", metadata)
    part("index.js", "application/javascript+module", Data(script.utf8))
    body.append(Data("--\(boundary)--\r\n".utf8))
    let (status, json) = try await call(
      "PUT", "/accounts/\(deployment.accountID)/workers/scripts/\(deployment.scriptName)", body: body,
      contentType: "multipart/form-data; boundary=\(boundary)")
    guard Self.succeeded(status, json) else { throw failFor(status, json) }
  }

  /// Polls `url` until `healthy` takes its answer, or the timeout.
  private func waitUntil(_ url: URL, healthy: (Data) -> Bool) async throws(Failure) {
    let deadline = ContinuousClock.now + healthTimeout
    while true {
      var request = URLRequest(url: url, timeoutInterval: 15)
      request.httpShouldHandleCookies = false
      if let (data, response) = try? await session.data(for: request),
        (response as? HTTPURLResponse)?.statusCode == 200, healthy(data)
      {
        return
      }
      if ContinuousClock.now >= deadline || Task.isCancelled {
        var failure = fail(.notReachable)
        failure.deployment = deployment
        throw failure
      }
      try? await Task.sleep(for: healthInterval)
    }
  }

  /// One API call: its status and JSON (nil when it isn't).
  private func call(_ method: String, _ path: String, body: Data? = nil, contentType: String? = nil)
    async throws(Failure) -> (Int, [String: Any]?)
  {
    step = method + " " + path
    if !deployment.accountID.isEmpty {
      step = step.replacingOccurrences(of: deployment.accountID, with: "{account}")
    }
    var request = URLRequest(url: URL(string: apiBase.absoluteString + path)!, timeoutInterval: 60)
    request.httpMethod = method
    request.httpShouldHandleCookies = false
    request.setValue("Bearer " + token, forHTTPHeaderField: "Authorization")
    if let body, let contentType {
      request.httpBody = body
      request.setValue(contentType, forHTTPHeaderField: "Content-Type")
    }
    let data: Data
    let response: URLResponse
    do {
      (data, response) = try await session.data(for: request)
    } catch {
      throw fail(.network)
    }
    guard let http = response as? HTTPURLResponse else { throw fail(.network) }
    status = http.statusCode
    return (status, try? JSONSerialization.jsonObject(with: data) as? [String: Any])
  }

  private func fail(_ kind: Failure.Kind, _ json: [String: Any]? = nil) -> Failure {
    Failure(kind: kind, detail: Self.apiMessage(json), step: step, status: status)
  }

  private func failFor(_ status: Int, _ json: [String: Any]?) -> Failure {
    fail(status == 401 || status == 403 ? .unauthorized : .api, json)
  }

  // MARK: Helpers

  static func succeeded(_ status: Int, _ json: [String: Any]?) -> Bool {
    (200..<300).contains(status) && (json?["success"] as? Bool) == true
  }

  /// A successful answer's `result.subdomain`.
  static func subdomain(_ status: Int, _ json: [String: Any]?) -> String? {
    guard succeeded(status, json), let name = (json?["result"] as? [String: Any])?["subdomain"] as? String,
      !name.isEmpty
    else { return nil }
    return name
  }

  /// The API's first error message, if any.
  static func apiMessage(_ json: [String: Any]?) -> String {
    ((json?["errors"] as? [Any])?.first as? [String: Any])?["message"] as? String ?? ""
  }

  static func randomLetters(_ n: Int) -> String {
    String((0..<n).map { _ in "abcdefghijklmnopqrstuvwxyz".randomElement()! })
  }
}
