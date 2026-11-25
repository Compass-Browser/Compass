// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CryptoKit
@testable import CompassSync
import Foundation
import Testing

/// Answers each request with the reply registered for its method and URL
/// (in order, for repeated ones), recording the requests with their bodies:
/// the fake Cloudflare API, token endpoint and sync server. The
/// counterparts of the desktop's cloudflare_deployer_unittest.cc,
/// cloudflare_oauth_unittest.cc and access_client_unittest.cc.
final class CloudflareStubProtocol: URLProtocol, @unchecked Sendable {
  struct Reply {
    var status: Int
    var body: String = ""
  }
  private static let lock = NSLock()
  nonisolated(unsafe) private static var replies: [String: [Reply]] = [:]
  nonisolated(unsafe) private static var recorded: [(URLRequest, Data)] = []

  static func session() -> URLSession {
    lock.withLock {
      replies = [:]
      recorded = []
    }
    let config = URLSessionConfiguration.ephemeral
    config.protocolClasses = [CloudflareStubProtocol.self]
    return URLSession(configuration: config)
  }

  static func on(_ method: String, _ url: String, _ status: Int = 200, _ body: String = "") {
    lock.withLock { replies[method + " " + url, default: []].append(Reply(status: status, body: body)) }
  }

  static var requests: [(URLRequest, Data)] { lock.withLock { recorded } }
  static func requests(_ method: String, _ url: String) -> [(URLRequest, Data)] {
    requests.filter { $0.0.httpMethod == method && $0.0.url?.absoluteString == url }
  }
  static func body(_ method: String, _ url: String) -> String {
    requests(method, url).last.map { String(decoding: $0.1, as: UTF8.self) } ?? ""
  }

  override class func canInit(with request: URLRequest) -> Bool { true }
  override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
  override func startLoading() {
    var body = request.httpBody ?? Data()
    if let stream = request.httpBodyStream {
      stream.open()
      var buffer = [UInt8](repeating: 0, count: 4096)
      while stream.hasBytesAvailable {
        let n = stream.read(&buffer, maxLength: buffer.count)
        if n <= 0 { break }
        body.append(buffer, count: n)
      }
      stream.close()
    }
    let key = (request.httpMethod ?? "GET") + " " + (request.url?.absoluteString ?? "")
    let reply: Reply? = Self.lock.withLock {
      Self.recorded.append((request, body))
      if var list = Self.replies[key], !list.isEmpty {
        let first = list.removeFirst()
        // The last answer stays for further requests (health polls).
        Self.replies[key] = list.isEmpty ? [first] : list
        return first
      }
      return nil
    }
    guard let reply else {
      client?.urlProtocol(self, didFailWithError: URLError(.cannotFindHost))
      return
    }
    let response = HTTPURLResponse(
      url: request.url!, statusCode: reply.status, httpVersion: nil,
      headerFields: ["Content-Type": "application/json"])!
    client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
    client?.urlProtocol(self, didLoad: Data(reply.body.utf8))
    client?.urlProtocolDidFinishLoading(self)
  }
  override func stopLoading() {}
}

private let api = "https://api.example.com/v4"
private let owner = "ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"
private let script = "export default {};"

private func json(_ text: String) -> [String: Any] {
  (try? JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any]) ?? [:]
}

/// One stub for all three: they run one test at a time.
@Suite(.serialized) enum CloudflareSuites {}

extension CloudflareSuites {
@Suite struct CloudflareDeployerTests {
  func deployer(_ session: URLSession, timeout: Duration = .seconds(5)) -> CloudflareDeployer {
    CloudflareDeployer(
      token: "the-token", script: script, apiBase: URL(string: api)!, session: session, healthTimeout: timeout,
      healthInterval: .milliseconds(10))
  }

  @Test func metadata() throws {
    var m = json(String(decoding: CloudflareDeployer.makeMetadata(owner: owner, migrationTag: ""), as: UTF8.self))
    #expect(m["main_module"] as? String == "index.js")
    var bindings = try #require(m["bindings"] as? [[String: Any]])
    #expect(bindings.count == 5)
    #expect(bindings[0]["name"] as? String == "CHAIN")
    #expect(bindings[3]["name"] as? String == "ROOM")
    // The owner, lower-case as the server compares it.
    #expect(bindings[4]["text"] as? String == owner.lowercased())
    // A new script: every class at once, as the latest tag.
    var migrations = try #require(m["migrations"] as? [String: Any])
    #expect(migrations["new_tag"] as? String == "v2")
    #expect(migrations["old_tag"] == nil)
    #expect((migrations["new_sqlite_classes"] as? [String])?.count == 4)

    // A script from before rooms: Room only, from v1.
    m = json(String(decoding: CloudflareDeployer.makeMetadata(owner: owner, migrationTag: "v1"), as: UTF8.self))
    migrations = try #require(m["migrations"] as? [String: Any])
    #expect(migrations["old_tag"] as? String == "v1")
    #expect(migrations["new_tag"] as? String == "v2")
    #expect(migrations["new_sqlite_classes"] as? [String] == ["Room"])

    m = json(String(decoding: CloudflareDeployer.makeMetadata(owner: owner, migrationTag: "v2"), as: UTF8.self))
    #expect(m["migrations"] == nil)

    // Push through the relay turned off at setup: the server starts with
    // it off. An update never sets it (the server keeps its variables).
    m = json(
      String(
        decoding: CloudflareDeployer.makeMetadata(owner: owner, migrationTag: "", pushRelay: false), as: UTF8.self))
    bindings = try #require(m["bindings"] as? [[String: Any]])
    #expect(bindings.count == 6)
    #expect(bindings[5]["name"] as? String == "COMPASS_SYNC_PUSH_DEFAULT")
    #expect(bindings[5]["text"] as? String == "off")
    m = json(
      String(decoding: CloudflareDeployer.makeMetadata(owner: "", migrationTag: "v2", pushRelay: false), as: UTF8.self))
    #expect((m["bindings"] as? [Any])?.count == 4)
    #expect(m["keep_bindings"] as? [String] == ["plain_text", "secret_text"])
  }

  /// A new account: no workers.dev subdomain, no scripts.
  @Test func newAccount() async throws {
    let session = CloudflareStubProtocol.session()
    let s = CloudflareStubProtocol.self
    s.on("GET", api + "/accounts?per_page=50", 200, #"{"success":true,"result":[{"id":"acc1","name":"Me"}]}"#)
    s.on(
      "GET", api + "/accounts/acc1/workers/subdomain", 404,
      #"{"success":false,"errors":[{"code":10007,"message":"no subdomain"}]}"#)
    s.on("PUT", api + "/accounts/acc1/workers/subdomain", 200, #"{"success":true,"result":{"subdomain":"compass-abc"}}"#)
    s.on("GET", api + "/accounts/acc1/workers/scripts", 200, #"{"success":true,"result":[]}"#)
    s.on("PUT", api + "/accounts/acc1/workers/scripts/compass-sync", 200, #"{"success":true,"result":{}}"#)
    s.on(
      "POST", api + "/accounts/acc1/workers/scripts/compass-sync/subdomain", 200,
      #"{"success":true,"result":{"enabled":true}}"#)
    s.on("GET", "https://compass-sync.compass-abc.workers.dev/healthz", 200, "ok\n")

    let progress = Lines()
    let deployment = try await deployer(session).deploy(owner: owner, progress: { progress.add($0) })
    #expect(deployment.accountID == "acc1")
    #expect(deployment.scriptName == "compass-sync")
    #expect(deployment.subdomain == "compass-abc")
    #expect(deployment.serverURL?.absoluteString == "https://compass-sync.compass-abc.workers.dev/v2")
    #expect(!progress.lines.isEmpty)

    // The name is random.
    #expect(s.body("PUT", api + "/accounts/acc1/workers/subdomain").contains(#""subdomain":"compass-"#))
    let upload = s.body("PUT", api + "/accounts/acc1/workers/scripts/compass-sync")
    #expect(upload.contains(#""new_sqlite_classes""#))
    #expect(upload.contains(script))
    #expect(upload.contains("Content-Type: application/javascript+module"))
    #expect(upload.contains("COMPASS_SYNC_ALLOWED_CHAINS"))
    let request = try #require(s.requests("PUT", api + "/accounts/acc1/workers/scripts/compass-sync").last?.0)
    #expect(request.value(forHTTPHeaderField: "Content-Type")?.hasPrefix("multipart/form-data; boundary=compass-") == true)
    // Every API call carries the token; the server's health doesn't.
    for (r, _) in s.requests {
      if r.url?.host() == "api.example.com" {
        #expect(r.value(forHTTPHeaderField: "Authorization") == "Bearer the-token")
      } else {
        #expect(r.value(forHTTPHeaderField: "Authorization") == nil)
      }
    }
  }

  /// An account that has a "compass-sync" already, and this chain's own
  /// script from an earlier try: it's updated, without the migration it
  /// ran.
  @Test func existingScripts() async throws {
    let session = CloudflareStubProtocol.session()
    let s = CloudflareStubProtocol.self
    s.on("GET", api + "/accounts?per_page=50", 200, #"{"success":true,"result":[{"id":"acc1"}]}"#)
    s.on("GET", api + "/accounts/acc1/workers/subdomain", 200, #"{"success":true,"result":{"subdomain":"me"}}"#)
    s.on(
      "GET", api + "/accounts/acc1/workers/scripts", 200,
      #"{"success":true,"result":[{"id":"compass-sync","migration_tag":"v1"},{"id":"compass-sync-abcdef","migration_tag":"v2"}]}"#
    )
    s.on("PUT", api + "/accounts/acc1/workers/scripts/compass-sync-abcdef", 200, #"{"success":true}"#)
    s.on("POST", api + "/accounts/acc1/workers/scripts/compass-sync-abcdef/subdomain", 200, #"{"success":true}"#)
    s.on("GET", "https://compass-sync-abcdef.me.workers.dev/healthz", 200, "ok")
    let deployment = try await deployer(session).deploy(owner: owner)
    #expect(deployment.scriptName == "compass-sync-abcdef")
    #expect(!s.body("PUT", api + "/accounts/acc1/workers/scripts/compass-sync-abcdef").contains("migrations"))
    #expect(s.requests("PUT", api + "/accounts/acc1/workers/subdomain").isEmpty)
  }

  @Test func pushRelayOff() async throws {
    let session = CloudflareStubProtocol.session()
    let s = CloudflareStubProtocol.self
    s.on("GET", api + "/accounts?per_page=50", 200, #"{"success":true,"result":[{"id":"acc1"}]}"#)
    s.on("GET", api + "/accounts/acc1/workers/subdomain", 200, #"{"success":true,"result":{"subdomain":"me"}}"#)
    s.on("GET", api + "/accounts/acc1/workers/scripts", 200, #"{"success":true,"result":[]}"#)
    s.on("PUT", api + "/accounts/acc1/workers/scripts/compass-sync", 200, #"{"success":true}"#)
    s.on("POST", api + "/accounts/acc1/workers/scripts/compass-sync/subdomain", 200, #"{"success":true}"#)
    s.on("GET", "https://compass-sync.me.workers.dev/healthz", 200, "ok")
    _ = try await deployer(session).deploy(owner: owner, pushRelay: false)
    #expect(s.body("PUT", api + "/accounts/acc1/workers/scripts/compass-sync").contains("COMPASS_SYNC_PUSH_DEFAULT"))
  }

  @Test func badToken() async {
    let session = CloudflareStubProtocol.session()
    CloudflareStubProtocol.on(
      "GET", api + "/accounts?per_page=50", 401,
      #"{"success":false,"errors":[{"code":1000,"message":"Invalid API Token"}]}"#)
    do {
      _ = try await deployer(session).deploy(owner: owner)
      Issue.record("deployed")
    } catch {
      #expect(error.kind == .unauthorized)
      #expect(error.status == 401)
      #expect(error.step == "GET /accounts?per_page=50")
      #expect(error.message.contains("Invalid API Token"))
    }
  }

  @Test func noAccount() async {
    let session = CloudflareStubProtocol.session()
    CloudflareStubProtocol.on("GET", api + "/accounts?per_page=50", 200, #"{"success":true,"result":[]}"#)
    do {
      _ = try await deployer(session).deploy(owner: owner)
      Issue.record("deployed")
    } catch {
      #expect(error.kind == .noAccount)
    }
  }

  @Test func apiRefusalKeepsItsMessage() async {
    let session = CloudflareStubProtocol.session()
    let s = CloudflareStubProtocol.self
    s.on("GET", api + "/accounts?per_page=50", 200, #"{"success":true,"result":[{"id":"acc1"}]}"#)
    s.on("GET", api + "/accounts/acc1/workers/subdomain", 200, #"{"success":true,"result":{"subdomain":"me"}}"#)
    s.on("GET", api + "/accounts/acc1/workers/scripts", 200, #"{"success":true,"result":[]}"#)
    s.on(
      "PUT", api + "/accounts/acc1/workers/scripts/compass-sync", 400,
      #"{"success":false,"errors":[{"code":10021,"message":"Script too big"}]}"#)
    do {
      _ = try await deployer(session).deploy(owner: owner)
      Issue.record("deployed")
    } catch {
      #expect(error.kind == .api)
      #expect(error.detail == "Script too big")
      // The account ID is left out of the step.
      #expect(error.step == "PUT /accounts/{account}/workers/scripts/compass-sync")
      #expect(error.message == "Cloudflare refused: Script too big")
    }
  }

  /// The new address doesn't resolve in time: the deployment is reported,
  /// but as not reachable.
  @Test func notReachable() async {
    let session = CloudflareStubProtocol.session()
    let s = CloudflareStubProtocol.self
    s.on("GET", api + "/accounts?per_page=50", 200, #"{"success":true,"result":[{"id":"acc1"}]}"#)
    s.on("GET", api + "/accounts/acc1/workers/subdomain", 200, #"{"success":true,"result":{"subdomain":"me"}}"#)
    s.on("GET", api + "/accounts/acc1/workers/scripts", 200, #"{"success":true,"result":[]}"#)
    s.on("PUT", api + "/accounts/acc1/workers/scripts/compass-sync", 200, #"{"success":true}"#)
    s.on("POST", api + "/accounts/acc1/workers/scripts/compass-sync/subdomain", 200, #"{"success":true}"#)
    // No answer registered for /healthz: the stub fails it as unresolved.
    do {
      _ = try await deployer(session, timeout: .milliseconds(100)).deploy(owner: owner)
      Issue.record("deployed")
    } catch {
      #expect(error.kind == .notReachable)
      #expect(error.deployment?.serverURL?.absoluteString == "https://compass-sync.me.workers.dev/v2")
      #expect(s.requests("GET", "https://compass-sync.me.workers.dev/healthz").count >= 2)
    }
  }

  /// An update: the account with the server's subdomain (not the first),
  /// its script uploaded keeping its variables and with the migration from
  /// v1, then waited for until it says the new version.
  @Test func update() async throws {
    let session = CloudflareStubProtocol.session()
    let s = CloudflareStubProtocol.self
    s.on("GET", api + "/accounts?per_page=50", 200, #"{"success":true,"result":[{"id":"acc0"},{"id":"acc1"}]}"#)
    s.on("GET", api + "/accounts/acc0/workers/subdomain", 200, #"{"success":true,"result":{"subdomain":"someone-else"}}"#)
    s.on("GET", api + "/accounts/acc1/workers/subdomain", 200, #"{"success":true,"result":{"subdomain":"me"}}"#)
    s.on(
      "GET", api + "/accounts/acc1/workers/scripts", 200,
      #"{"success":true,"result":[{"id":"compass-sync","migration_tag":"v1"}]}"#)
    s.on("PUT", api + "/accounts/acc1/workers/scripts/compass-sync", 200, #"{"success":true}"#)
    // The old version answers a moment longer.
    let info = "https://compass-sync.me.workers.dev/v2/compass/info"
    s.on("GET", info, 200, #"{"login":null}"#)
    s.on("GET", info, 200, #"{"login":null,"server":{"kind":"cloudflare","version":2}}"#)

    let deployment = try await deployer(session).update(scriptName: "compass-sync", subdomain: "me", version: 2)
    #expect(deployment.accountID == "acc1")
    #expect(s.requests("GET", info).count == 2)
    let upload = s.body("PUT", api + "/accounts/acc1/workers/scripts/compass-sync")
    #expect(upload.contains(#""keep_bindings""#))
    #expect(!upload.contains("COMPASS_SYNC_ALLOWED_CHAINS"))
    #expect(upload.contains(#""old_tag":"v1""#))
    // Its route is on already.
    #expect(s.requests("POST", api + "/accounts/acc1/workers/scripts/compass-sync/subdomain").isEmpty)
  }

  @Test func updateNotInThisAccount() async {
    let session = CloudflareStubProtocol.session()
    let s = CloudflareStubProtocol.self
    s.on("GET", api + "/accounts?per_page=50", 200, #"{"success":true,"result":[{"id":"acc0"}]}"#)
    s.on("GET", api + "/accounts/acc0/workers/subdomain", 200, #"{"success":true,"result":{"subdomain":"someone-else"}}"#)
    do {
      _ = try await deployer(session).update(scriptName: "compass-sync", subdomain: "me", version: 2)
      Issue.record("updated")
    } catch {
      #expect(error.kind == .notFound)
      #expect(!error.message.isEmpty)
    }
  }

  @Test func infoAndNames() throws {
    #expect(ServerInfo.parse(Data(#"{"login":null,"server":{"kind":"cloudflare","version":2}}"#.utf8))?.version == 2)
    #expect(ServerInfo.parse(Data(#"{"server":{"kind":"cloudflare","version":2}}"#.utf8))?.kind == "cloudflare")
    // A Go server, or a Cloudflare server from before versions.
    #expect(ServerInfo.parse(Data(#"{"login":null}"#.utf8)) == ServerInfo())
    #expect(ServerInfo.parse(Data("404 page not found".utf8)) == nil)

    let name = try #require(CloudflareServerName(serverURL: URL(string: "https://compass-sync.compass-abc.workers.dev/v2")!))
    #expect(name.script == "compass-sync")
    #expect(name.subdomain == "compass-abc")
    #expect(CloudflareServerName(serverURL: URL(string: "https://sync.example.com/v2")!) == nil)
    #expect(CloudflareServerName(serverURL: URL(string: "https://workers.dev/v2")!) == nil)
    #expect(CloudflareServerName(serverURL: URL(string: "https://a.b.c.workers.dev/v2")!) == nil)
    #expect(CloudflareServerName(serverURL: URL(string: "http://a.b.workers.dev/v2")!) == nil)
  }
}

final class Lines: @unchecked Sendable {
  private let lock = NSLock()
  private var list: [String] = []
  func add(_ line: String) { lock.withLock { list.append(line) } }
  var lines: [String] { lock.withLock { list } }
}

}

extension CloudflareSuites {
@Suite struct CloudflareOAuthTests {
  func query(_ url: URL, _ name: String) -> String? {
    CloudflareOAuth.formItems(url).first { $0.name == name }?.value
  }

  @Test func request() {
    let r = CloudflareOAuth.makeRequest()
    #expect(r.verifier.count == 64)
    #expect(r.state.count >= 8)
    #expect(r.url.absoluteString.hasPrefix(CloudflareOAuth.authorizeURL + "?"))
    #expect(query(r.url, "response_type") == "code")
    #expect(query(r.url, "client_id") == CloudflareOAuth.clientID)
    #expect(query(r.url, "redirect_uri") == CloudflareOAuth.redirectURI)
    #expect(query(r.url, "scope") == CloudflareOAuth.scopes)
    #expect(query(r.url, "state") == r.state)
    #expect(query(r.url, "code_challenge_method") == "S256")
    let challenge = Data(SHA256.hash(data: Data(r.verifier.utf8))).base64URLEncodedString(padded: false)
    #expect(query(r.url, "code_challenge") == challenge)
    // The redirect URI and scopes are escaped as Chromium's
    // AppendQueryParameter() does.
    #expect(r.url.absoluteString.contains("redirect_uri=https%3A%2F%2Fcompassbrowser.org%2Foauth%2Fcloudflare"))
    #expect(r.url.absoluteString.contains("scope=account-settings.read+workers-scripts.admin"))
    // Each request has its own secrets.
    #expect(r.verifier != CloudflareOAuth.makeRequest().verifier)
  }

  @Test func redirect() {
    #expect(CloudflareOAuth.isRedirect(URL(string: "https://compassbrowser.org/oauth/cloudflare?code=a&state=b")!))
    #expect(!CloudflareOAuth.isRedirect(URL(string: "http://compassbrowser.org/oauth/cloudflare?code=a")!))
    #expect(!CloudflareOAuth.isRedirect(URL(string: "https://evil.example/oauth/cloudflare?code=a")!))
    #expect(!CloudflareOAuth.isRedirect(URL(string: "https://compassbrowser.org/oauth/other")!))
    #expect(!CloudflareOAuth.isRedirect(URL(string: "https://compassbrowser.org:8443/oauth/cloudflare")!))

    var reply = CloudflareOAuth.parseRedirect(
      URL(string: "https://compassbrowser.org/oauth/cloudflare?code=abc&state=s1234567")!, expectedState: "s1234567")
    #expect(reply.code == "abc")
    reply = CloudflareOAuth.parseRedirect(
      URL(string: "https://compassbrowser.org/oauth/cloudflare?code=abc&state=other")!, expectedState: "s1234567")
    #expect(reply.code == nil)
    #expect(reply.error == "state")
    reply = CloudflareOAuth.parseRedirect(
      URL(string: "https://compassbrowser.org/oauth/cloudflare?error=access_denied&state=s1234567")!,
      expectedState: "s1234567")
    #expect(reply.code == nil)
    #expect(reply.error == "access_denied")
    reply = CloudflareOAuth.parseRedirect(
      URL(string: "https://compassbrowser.org/oauth/cloudflare?state=s1234567")!, expectedState: "s1234567")
    #expect(reply.error == "no_code")
  }

  @Test func exchange() async {
    let session = CloudflareStubProtocol.session()
    let s = CloudflareStubProtocol.self
    let url = CloudflareOAuth.tokenURL.absoluteString
    s.on("POST", url, 200, #"{"access_token":"tok","token_type":"bearer"}"#)
    var token = await CloudflareOAuth.exchange(code: "the code", verifier: "the-verifier", session: session)
    #expect(token == "tok")
    let body = s.body("POST", url)
    #expect(body.contains("grant_type=authorization_code"))
    #expect(body.contains("code=the+code"))
    #expect(body.contains("code_verifier=the-verifier"))
    #expect(body.contains("client_id=" + CloudflareOAuth.clientID))
    #expect(body.contains("redirect_uri=https%3A%2F%2Fcompassbrowser.org%2Foauth%2Fcloudflare"))
    #expect(
      s.requests("POST", url).last?.0.value(forHTTPHeaderField: "Content-Type") == "application/x-www-form-urlencoded")

    // A refusal: no token.
    _ = CloudflareStubProtocol.session()
    s.on("POST", url, 400, #"{"error":"invalid_grant"}"#)
    token = await CloudflareOAuth.exchange(code: "c", verifier: "v", session: session)
    #expect(token == nil)
  }
}

}

extension CloudflareSuites {
@Suite struct AccessClientTests {
  let server = "https://compass-sync.me.workers.dev/v2"

  func client(_ session: URLSession) -> AccessClient {
    AccessClient(serverURL: URL(string: server)!, session: session)
  }

  @Test func parseInviteLink() throws {
    var link = try #require(ServerInviteLink("  https://compass-sync.me.workers.dev/invite#abcdefghijklmnopqrstuvwxyz "))
    #expect(link.serverURL.absoluteString == server)
    #expect(link.code == "abcdefghijklmnopqrstuvwxyz")

    // Under a path prefix, and the code's case doesn't matter.
    link = try #require(ServerInviteLink("https://example.org/sync/invite#ABCDEFGHIJ234567"))
    #expect(link.serverURL.absoluteString == "https://example.org/sync/v2")
    #expect(link.code == "abcdefghij234567")

    #expect(ServerInviteLink("https://example.org/invite") == nil)
    #expect(ServerInviteLink("https://example.org/invite#short") == nil)
    #expect(ServerInviteLink("https://example.org/other#abcdefghijklmnop") == nil)
    #expect(ServerInviteLink("ftp://example.org/invite#abcdefghijklmnop") == nil)
    #expect(ServerInviteLink("https://example.org/invite#abcdefghij-klmn") == nil)
    #expect(ServerInviteLink("7-482913") == nil)
  }

  @Test func list() async throws {
    let session = CloudflareStubProtocol.session()
    CloudflareStubProtocol.on(
      "GET", server + "/compass/access", 200,
      #"""
      {"owners":["aa"],
       "members":[{"chain":"bb","name":"Sam","added_at":1700000000,"linked":2}],
       "invites":[{"code":"cc","name":"Kim","expires_in":3600}],
       "settings":{"push_relay":false,"push_relay_url":"https://r.example"}}
      """#)
    let people = try await client(session).list(token: "tok")
    #expect(CloudflareStubProtocol.requests.last?.0.value(forHTTPHeaderField: "Authorization") == "Bearer tok")
    #expect(people.members.count == 1)
    #expect(people.members[0].name == "Sam")
    #expect(people.members[0].linked == 2)
    #expect(people.members[0].added == Date(timeIntervalSince1970: 1_700_000_000))
    #expect(people.invites.count == 1)
    #expect(people.invites[0].code == "cc")
    #expect(abs(people.invites[0].expires.timeIntervalSinceNow - 3600) < 60)
    #expect(people.pushRelay == false)
    #expect(people.pushRelayURL == "https://r.example")
    #expect(people.pushRelayHost == "r.example")
  }

  @Test func listFromAServerBeforePush() async throws {
    let session = CloudflareStubProtocol.session()
    CloudflareStubProtocol.on("GET", server + "/compass/access", 200, #"{"owners":["aa"],"members":[],"invites":[]}"#)
    let people = try await client(session).list(token: "tok")
    #expect(people.pushRelay == nil)
    #expect(people.pushRelayURL == "")
  }

  @Test func setPushRelay() async throws {
    let session = CloudflareStubProtocol.session()
    CloudflareStubProtocol.on("PUT", server + "/compass/access/settings", 200, #"{"push_relay":false}"#)
    try await client(session).setPushRelay(token: "tok", on: false)
    #expect(CloudflareStubProtocol.body("PUT", server + "/compass/access/settings") == #"{"push_relay":false}"#)
  }

  @Test func listOnAServerWithoutAccessOrAsAFriend() async {
    for (status, want) in [(404, AccessClient.Failure.unsupported), (403, .notOwner)] {
      let session = CloudflareStubProtocol.session()
      CloudflareStubProtocol.on("GET", server + "/compass/access", status, #"{"error":"x"}"#)
      do {
        _ = try await client(session).list(token: "tok")
        Issue.record("listed")
      } catch {
        #expect(error == want)
      }
    }
  }

  @Test func createInvite() async throws {
    let session = CloudflareStubProtocol.session()
    CloudflareStubProtocol.on(
      "POST", server + "/compass/access/invites", 201, #"{"code":"abc","link":"https://s.example/invite#abc"}"#)
    let link = try await client(session).createInvite(token: "tok", name: "Sam")
    #expect(link == "https://s.example/invite#abc")
    #expect(CloudflareStubProtocol.body("POST", server + "/compass/access/invites") == #"{"name":"Sam"}"#)
  }

  @Test func joinErrors() async {
    let cases: [(Int, String, AccessClient.Failure?)] = [
      (204, "", nil),
      (404, #"{"error":"no_such_invite"}"#, .noSuchInvite),
      (404, "404 page not found", .unsupported),
      (410, #"{"error":"invite_used_or_expired"}"#, .inviteGone),
      (429, #"{"error":"too_many"}"#, .tooMany),
      (500, "", .failed),
    ]
    for (status, body, want) in cases {
      let session = CloudflareStubProtocol.session()
      CloudflareStubProtocol.on("POST", server + "/compass/access/join", status, body)
      do {
        try await client(session).join(token: "tok", code: "code")
        #expect(want == nil, "\(status)")
      } catch {
        #expect(error == want, "\(status)")
        #expect(!error.message.isEmpty)
      }
    }
    #expect(CloudflareStubProtocol.body("POST", server + "/compass/access/join") == #"{"code":"code"}"#)
  }

  @Test func removeEscapesTheTarget() async throws {
    let session = CloudflareStubProtocol.session()
    CloudflareStubProtocol.on("DELETE", server + "/compass/access/a%2Fb", 204)
    try await client(session).remove(token: "tok", target: "a/b")
    #expect(CloudflareStubProtocol.requests.last?.0.httpMethod == "DELETE")
  }

  @Test func info() async {
    let session = CloudflareStubProtocol.session()
    CloudflareStubProtocol.on("GET", server + "/compass/info", 200, #"{"login":null,"server":{"kind":"cloudflare","version":3}}"#)
    #expect(await client(session).info() == ServerInfo(kind: "cloudflare", version: 3))
    #expect(CloudflareStubProtocol.requests.last?.0.value(forHTTPHeaderField: "Authorization") == nil)
  }

  /// A chain token signs the join (the joining chain's own).
  @Test func joinIsSignedWithTheChain() async throws {
    let session = CloudflareStubProtocol.session()
    CloudflareStubProtocol.on("POST", server + "/compass/access/join", 204)
    let chain = try Chain(seed: Data(0..<16))
    try await client(session).join(token: chain.accessToken(), code: "abcdefghijk")
    let header = try #require(CloudflareStubProtocol.requests.last?.0.value(forHTTPHeaderField: "Authorization"))
    let b64 = header.dropFirst(7).replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
    let plain = String(decoding: Data(base64Encoded: b64) ?? Data(), as: UTF8.self)
    #expect(plain.split(separator: "|").last.map(String.init) == chain.chainID.uppercased())
  }
}
}

/// The access client against a real Cloudflare server (`wrangler dev`, or a
/// deployed one) whose owner is `COMPASS_CF_OWNER_SEED`'s chain, as the
/// server's own tests take them (server/cloudflare/README.md):
///   wrangler dev --var COMPASS_SYNC_ALLOWED_CHAINS:<that chain ID>
///   COMPASS_CF_URL=http://127.0.0.1:8787 COMPASS_CF_OWNER_SEED=<32 hex> \
///     compass ios test --filter AccessClientServerTests
/// Skipped without them.
@Suite struct AccessClientServerTests {
  static let url = ProcessInfo.processInfo.environment["COMPASS_CF_URL"]
  static let seed = ProcessInfo.processInfo.environment["COMPASS_CF_OWNER_SEED"].flatMap { Data(hex: $0) }

  @Test(.enabled(if: url != nil && seed != nil)) func inviteJoinListRemove() async throws {
    let server = URL(string: Self.url! + "/v2")!
    let owner = try Chain(seed: Self.seed!)
    let client = AccessClient(serverURL: server)
    #expect(await client.info()?.kind == "cloudflare")

    let friend = try Chain(seed: Chain.generateSeed())
    // Not on the list yet: refused, and not an owner.
    await #expect(throws: AccessClient.Failure.unauthorized) { try await client.list(token: friend.accessToken()) }
    let link = try await client.createInvite(token: owner.accessToken(), name: "Sam")
    let invite = try #require(ServerInviteLink(link))
    try await client.join(token: friend.accessToken(), code: invite.code)
    // Single use.
    await #expect(throws: AccessClient.Failure.inviteGone) {
      try await client.join(token: Chain(seed: Chain.generateSeed()).accessToken(), code: invite.code)
    }
    // (A chain on the list already is let in whatever the code.)
    await #expect(throws: AccessClient.Failure.noSuchInvite) {
      try await client.join(token: Chain(seed: Chain.generateSeed()).accessToken(), code: "abcdefghijklmnop")
    }
    // A friend isn't an owner.
    await #expect(throws: AccessClient.Failure.notOwner) { try await client.list(token: friend.accessToken()) }

    let open = try #require(ServerInviteLink(try await client.createInvite(token: owner.accessToken(), name: "Kim")))
    var people = try await client.list(token: owner.accessToken())
    #expect(people.members.contains { $0.chain == friend.chainID && $0.name == "Sam" })
    #expect(people.invites.contains { $0.code == open.code && $0.name == "Kim" })
    #expect(people.pushRelay != nil)

    let before = people.pushRelay!
    try await client.setPushRelay(token: owner.accessToken(), on: !before)
    #expect(try await client.list(token: owner.accessToken()).pushRelay == !before)
    try await client.setPushRelay(token: owner.accessToken(), on: before)

    try await client.remove(token: owner.accessToken(), target: open.code)
    try await client.remove(token: owner.accessToken(), target: friend.chainID)
    people = try await client.list(token: owner.accessToken())
    #expect(!people.members.contains { $0.chain == friend.chainID })
    #expect(!people.invites.contains { $0.code == open.code })
  }
}
