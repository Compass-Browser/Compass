// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassEasels
import CompassModel
@testable import CompassSync
import Foundation
import SyncProto
import Testing

@testable import CompassRooms

/// The repo's Go sync server, per test (as CompassSyncTests' TestServer),
/// optionally with company sign-in against its fake identity provider.
final class RoomTestServer: @unchecked Sendable {
  static let binary = ProcessInfo.processInfo.environment["COMPASS_SYNC_SERVER"]
  static var available: Bool { binary.map { FileManager.default.isExecutableFile(atPath: $0) } ?? false }

  let process = Process()
  let directory: URL
  let url: URL
  let port: Int

  init(oidc: Bool = false) async throws {
    directory = FileManager.default.temporaryDirectory.appendingPathComponent("compass-rooms-test-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
    port = Int.random(in: 20000..<40000)
    url = URL(string: "http://127.0.0.1:\(port)/v2")!
    process.executableURL = URL(fileURLWithPath: Self.binary!)
    var environment = [
      "COMPASS_SYNC_ADDR": "127.0.0.1:\(port)",
      "COMPASS_SYNC_DB": directory.appendingPathComponent("sync.db").path,
      "ENV": "local",
      "PATH": "/usr/bin:/bin",
    ]
    if oidc {
      environment["COMPASS_SYNC_DEV_FAKE_OIDC"] = "1"
      environment["COMPASS_SYNC_PUBLIC_HOST"] = "127.0.0.1:\(port)"
    }
    process.environment = environment
    process.standardOutput = FileHandle(forWritingAtPath: "/dev/null")
    let log = directory.appendingPathComponent("server.log")
    FileManager.default.createFile(atPath: log.path, contents: nil)
    process.standardError = try FileHandle(forWritingTo: log)
    try process.run()
    let health = URL(string: "http://127.0.0.1:\(port)/healthz")!
    for _ in 0..<200 {
      if let (_, response) = try? await URLSession.shared.data(from: health),
        (response as? HTTPURLResponse)?.statusCode == 200
      {
        return
      }
      try await Task.sleep(for: .milliseconds(50))
    }
    throw URLError(.cannotConnectToHost)
  }

  var log: String { (try? String(contentsOf: directory.appendingPathComponent("server.log"), encoding: .utf8)) ?? "" }

  /// Every byte of the database, for the no-plaintext check.
  var databaseBytes: Data {
    var out = Data()
    for name in ["sync.db", "sync.db-wal"] {
      out += (try? Data(contentsOf: directory.appendingPathComponent(name))) ?? Data()
    }
    return out
  }

  /// Signs `chain` in as `email` through the fake identity provider, as a
  /// browser would: start, the provider's form, the consent's Allow.
  func signIn(_ chain: Chain, email: String, name: String) async throws {
    let base = URL(string: "http://127.0.0.1:\(port)")!
    var start = URLRequest(url: base.appendingPathComponent("v2/compass/login"))
    start.httpMethod = "POST"
    start.setValue("Bearer " + chain.accessToken(), forHTTPHeaderField: "Authorization")
    start.setValue("application/json", forHTTPHeaderField: "Content-Type")
    start.httpBody = Data(#"{"device_name":"Test","return":"tab"}"#.utf8)
    let (created, _) = try await URLSession.shared.data(for: start)
    let login = try JSONSerialization.jsonObject(with: created) as! [String: Any]
    let id = login["id"] as! String
    // A browser: redirects followed by hand, carrying the login cookie.
    let browser = Browser()
    // /start → the provider's form.
    let (formData, formURL) = try await browser.load(URLRequest(url: URL(string: login["url"] as! String)!))
    var fields = Self.hiddenFields(String(decoding: formData, as: UTF8.self))
    fields["email"] = email
    fields["name"] = name
    fields["email_verified"] = "true"
    var authorize = URLRequest(url: formURL)
    authorize.httpMethod = "POST"
    authorize.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
    authorize.httpBody = Data(Self.formBody(fields).utf8)
    // → the callback's consent page.
    let (consentData, _) = try await browser.load(authorize)
    let csrf = Self.hiddenFields(String(decoding: consentData, as: UTF8.self))["csrf"] ?? ""
    var approve = URLRequest(url: base.appendingPathComponent("v2/compass/login/\(id)/approve"))
    approve.httpMethod = "POST"
    approve.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
    approve.httpBody = Data(Self.formBody(["csrf": csrf]).utf8)
    _ = try await browser.load(approve)
    var poll = URLRequest(url: base.appendingPathComponent("v2/compass/login/\(id)").appending(queryItems: [.init(name: "wait", value: "0")]))
    poll.setValue("Bearer " + chain.accessToken(), forHTTPHeaderField: "Authorization")
    let (status, _) = try await URLSession.shared.data(for: poll)
    let answer = try JSONSerialization.jsonObject(with: status) as? [String: Any]
    guard answer?["status"] as? String == "done" else { throw URLError(.userAuthenticationRequired) }
  }

  /// Follows redirects itself and keeps cookies (by name, any path).
  final class Browser: NSObject, URLSessionTaskDelegate, @unchecked Sendable {
    var cookies: [String: String] = [:]
    lazy var session = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: nil)

    func urlSession(
      _ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
      newRequest request: URLRequest
    ) async -> URLRequest? { nil }

    func load(_ first: URLRequest) async throws -> (Data, URL) {
      var request = first
      for _ in 0..<10 {
        request.httpShouldHandleCookies = false
        if !cookies.isEmpty {
          request.setValue(cookies.map { "\($0.key)=\($0.value)" }.joined(separator: "; "), forHTTPHeaderField: "Cookie")
        }
        let (data, response) = try await session.data(for: request)
        let http = response as! HTTPURLResponse
        for cookie in HTTPCookie.cookies(
          withResponseHeaderFields: http.allHeaderFields as? [String: String] ?? [:], for: request.url!)
        {
          cookies[cookie.name] = cookie.value
        }
        guard (300..<400).contains(http.statusCode), let location = http.value(forHTTPHeaderField: "Location"),
          let next = URL(string: location, relativeTo: request.url)?.absoluteURL
        else { return (data, request.url!) }
        request = URLRequest(url: next)
      }
      throw URLError(.httpTooManyRedirects)
    }
  }

  static func hiddenFields(_ html: String) -> [String: String] {
    var out: [String: String] = [:]
    let pattern = try! NSRegularExpression(pattern: #"<input type="hidden" name="([^"]+)" value="([^"]*)">"#)
    for match in pattern.matches(in: html, range: NSRange(html.startIndex..., in: html)) {
      let name = String(html[Range(match.range(at: 1), in: html)!])
      let value = String(html[Range(match.range(at: 2), in: html)!])
        .replacingOccurrences(of: "&amp;", with: "&").replacingOccurrences(of: "&#43;", with: "+")
        .replacingOccurrences(of: "&#34;", with: "\"").replacingOccurrences(of: "&#39;", with: "'")
        .replacingOccurrences(of: "&lt;", with: "<").replacingOccurrences(of: "&gt;", with: ">")
      out[name] = value
    }
    return out
  }

  static func formBody(_ fields: [String: String]) -> String {
    var allowed = CharacterSet.alphanumerics
    allowed.insert(charactersIn: "-._~")
    return fields.map {
      "\($0.key.addingPercentEncoding(withAllowedCharacters: allowed)!)=\($0.value.addingPercentEncoding(withAllowedCharacters: allowed)!)"
    }.joined(separator: "&")
  }

  deinit {
    process.terminate()
    // Polled, as TestServer does: waitUntilExit() can hang parallel tests.
    let deadline = Date().addingTimeInterval(5)
    while process.isRunning && Date() < deadline { usleep(10_000) }
    try? FileManager.default.removeItem(at: directory)
  }
}

@Suite(.enabled(if: RoomTestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
@MainActor
struct RoomServerTests {
  let alice = try! Chain(seed: Chain.generateSeed())
  let bob = try! Chain(seed: Chain.generateSeed())

  /// Alice shares a folder by link, Bob joins as an editor: edits flow both
  /// ways; Alice's other device joins from her chain's mount; nothing of it
  /// stays in Alice's chain; and the server holds no plaintext.
  @Test func shareAFolderByLink() async throws {
    let server = try await RoomTestServer()
    let a = await RoomDevice.make(chain: alice, server: server.url, name: "Alice")
    let work = a.store.addSpace(name: "Work")
    let team = a.store.addFolder(name: "Team Plans", to: work.pinned)!
    a.store.addTab(url: "https://secret-plans.example/", title: "Quarterly secret plans", to: team)
    try await a.sync()
    #expect(a.bridge.mirror[team.id] != nil)

    let room = try await a.rooms.share(kind: .folder, rootUUID: team.id, name: team.title, account: a.account)
    // The room has the ID Alice chose, and her epoch 1 envelope (sent with
    // the creation, nothing replaced) opens to the key she keeps.
    let created = try await a.rooms.client(for: room)!.room(room.roomID)
    #expect(created.envelopes.count == 1)
    #expect(
      RoomCrypto.openEnvelope(
        identity: a.account.identity, roomID: room.roomIDBytes, epoch: 1, envelope: created.envelopes[0].sealed)
        == room.keys[1])
    // A taken ID is refused.
    await #expect(throws: RoomError.roomExists) {
      try await a.rooms.client(for: room)!.createRoom(
        id: room.roomIDBytes, kind: .folder, identityPub: a.account.identity.publicKey,
        envelope: try RoomCrypto.sealEnvelope(
          recipientPublicKey: a.account.identity.publicKey, roomID: room.roomIDBytes, epoch: 1,
          roomKey: room.keys[1]!))
    }
    try await a.sync()
    // The chain has the mount, not the folder.
    #expect(a.bridge.mirror[team.id] == nil)
    #expect(a.bridge.roomMounts().map(\.roomID) == [room.roomID])
    let link = try await a.rooms.inviteLink(room, role: .editor, expires: Date().addingTimeInterval(3600), uses: 2)
    let parsed = try #require(RoomInviteLink(link: link))
    #expect(parsed.name == "Team Plans" && parsed.kind == .folder)

    let b = await RoomDevice.make(chain: bob, server: server.url, name: "Bob")
    let home = b.store.addSpace(name: "Home")
    try await b.sync()
    let joined = try await b.rooms.join(parsed, account: b.account, space: home)
    #expect(joined.role == .editor)
    let shared = try #require(b.store.node(id: team.id))
    #expect(shared.parent === home.pinned)
    #expect(shared.children.map(\.url) == ["https://secret-plans.example/"])
    // Names: Alice's own, as Bob sees it.
    _ = try await joined.refresh(b.rooms.client(for: joined)!, identity: b.account.identity)
    let owner = try #require(joined.info?.member(alice.chainID))
    #expect(b.rooms.displayName(of: owner, in: joined) == "Alice")

    // Bob adds a tab; Alice gets it.
    b.store.addTab(url: "https://from-bob.example/", title: "From Bob", to: shared)
    await b.rooms.sync(joined)
    await a.rooms.syncAll()
    #expect(team.children.map(\.url) == ["https://secret-plans.example/", "https://from-bob.example/"])
    // Alice renames; Bob sees it.
    a.store.rename(team, customTitle: "Team")
    await a.rooms.syncAll()
    await b.rooms.syncAll()
    #expect(shared.title == "Team")
    // Bob's placement is his: Alice's folder didn't move.
    #expect(team.parent === work.pinned)

    // Alice's iPad joins from her chain.
    let a2 = await RoomDevice.make(chain: alice, server: server.url, first: false)
    try await a2.sync()
    #expect(a2.rooms.rooms[room.roomID] != nil)
    await a2.rooms.syncAll()
    let onPad = try #require(a2.store.node(id: team.id))
    #expect(onPad.children.count == 2 && onPad.title == "Team")
    #expect(a2.store.space(of: onPad)?.id == work.id)

    // The server never saw a title or URL.
    let bytes = server.databaseBytes
    for secret in ["secret-plans", "Quarterly secret", "from-bob", "Team Plans"] {
      #expect(bytes.range(of: Data(secret.utf8)) == nil, "\(secret) is in the database")
    }
  }

  /// An easel shared with a viewer: the viewer reads, can't write (the
  /// server refuses, its edits go back); removed, it keeps its copy and
  /// can't read what's written after the rotation.
  @Test func shareAnEaselWithAViewerThenRemoveIt() async throws {
    let server = try await RoomTestServer()
    let a = await RoomDevice.make(chain: alice, server: server.url)
    let space = a.store.addSpace(name: "Work")
    try await a.sync()
    let board = try a.easels.createEasel(title: "Roadmap").get()
    a.store.addTab(url: EaselURL.url(board.uuid), title: "Roadmap", to: space.pinned)
    var note = EaselObject()
    note.uuid = UUID().uuidString.lowercased()
    note.easelUuid = board.uuid
    note.width = 100
    note.height = 50
    note.text.text = "Ship it"
    _ = try a.easels.save(note).get()
    let png = Data(base64Encoded: "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==")!
    let sha = try a.easels.addAsset(png, mimeType: "image/png").get()
    var picture = EaselObject()
    picture.uuid = UUID().uuidString.lowercased()
    picture.easelUuid = board.uuid
    picture.width = 10
    picture.height = 10
    picture.image.asset = sha
    picture.image.pixelWidth = 1
    picture.image.pixelHeight = 1
    _ = try a.easels.save(picture).get()
    try await a.sync()

    let room = try await a.rooms.share(kind: .easel, rootUUID: board.uuid, name: "Roadmap", account: a.account)
    try await a.sync()
    #expect(a.bridge.mirror[board.uuid] == nil && a.bridge.mirror[note.uuid] == nil)
    let link = try await a.rooms.inviteLink(room, role: .viewer, expires: nil, uses: nil)

    let b = await RoomDevice.make(chain: bob, server: server.url)
    let home = b.store.addSpace(name: "Home")
    try await b.sync()
    let joined = try await b.rooms.join(RoomInviteLink(link: link)!, account: b.account, space: home)
    #expect(joined.role == .viewer)
    #expect(b.easels.easel(board.uuid)?.title == "Roadmap")
    #expect(b.easels.object(note.uuid)?.text.text == "Ship it")
    #expect(b.easels.assetData(sha) == png)
    // The board shows as a pinned tab where Bob put it.
    let tab = try #require(b.store.node(id: joined.easelTabID))
    #expect(tab.parent === home.pinned && tab.url == EaselURL.url(board.uuid))

    // A viewer's write is refused by the server…
    let client = b.rooms.client(for: joined)!
    let ciphertext = try RoomCrypto.encryptRecord(
      roomKey: joined.keys[1]!, roomID: joined.roomIDBytes, recordID: note.uuid, epoch: 1, plaintext: Data())
    await #expect(throws: RoomError.refused(403, "forbidden")) {
      try await client.write(joined.roomID, records: [RoomWrite(id: note.uuid, epoch: 1, ciphertext: ciphertext)])
    }
    // …and its edits here go back.
    var edited = b.easels.object(note.uuid)!
    edited.text.text = "Bob was here"
    _ = b.easels.save(edited)
    await b.rooms.sync(joined)
    #expect(b.easels.object(note.uuid)?.text.text == "Ship it")

    // Alice edits; Bob reads it.
    var changed = a.easels.object(note.uuid)!
    changed.text.text = "Ship it Friday"
    _ = a.easels.save(changed)
    await a.rooms.syncAll()
    await b.rooms.syncAll()
    #expect(b.easels.object(note.uuid)?.text.text == "Ship it Friday")

    // Alice removes Bob: the key rotates.
    try await a.rooms.remove(member: bob.chainID, from: room)
    #expect(room.epoch == 2 && room.keys[2] != nil)
    changed.text.text = "After Bob"
    _ = a.easels.save(changed)
    await a.rooms.syncAll()
    // The new record is of epoch 2, which Bob's keys don't open.
    let aliceClient = a.rooms.client(for: room)!
    let records = try await aliceClient.changes(room.roomID, since: 0).records
    let latest = try #require(records.first { $0.id == note.uuid })
    #expect(latest.epoch == 2)
    #expect(
      RoomCrypto.decryptRecord(
        roomKey: joined.keys[1]!, roomID: joined.roomIDBytes, recordID: note.uuid, epoch: 2, sealed: latest.ciphertext!)
        == nil)
    #expect(joined.keys[2] == nil)
    // Bob is refused, keeps his copy unshared, and sees why.
    await #expect(throws: RoomError.removed) { try await client.changes(joined.roomID, since: 0) }
    await b.rooms.sync(joined)
    #expect(b.rooms.rooms[joined.roomID] == nil)
    #expect(b.rooms.notices.count == 1)
    #expect(b.easels.object(note.uuid)?.text.text == "Ship it Friday")
    #expect(!b.rooms.isRoomEasel(board.uuid))
    try await b.sync()
    #expect(b.bridge.roomMounts().isEmpty)
    // His copy now syncs in his own chain.
    #expect(b.bridge.mirror[board.uuid] != nil)
  }

  /// On a company sign-in server, Alice invites Bob by email: he finds the
  /// invitation, accepts it and gets the folder.
  @Test func inviteByEmail() async throws {
    let server = try await RoomTestServer(oidc: true)
    try await server.signIn(alice, email: "alice@example.com", name: "Alice Example")
    try await server.signIn(bob, email: "bob@example.com", name: "Bob Example")
    let a = await RoomDevice.make(chain: alice, server: server.url)
    let work = a.store.addSpace(name: "Work")
    let folder = a.store.addFolder(name: "Specs", to: work.pinned)!
    a.store.addTab(url: "https://specs.example/", title: "Spec", to: folder)
    try await a.sync()
    let b = await RoomDevice.make(chain: bob, server: server.url)
    let home = b.store.addSpace(name: "Home")
    try await b.sync()
    await a.rooms.checkServer("")
    await b.rooms.checkServer("")

    let room = try await a.rooms.share(kind: .folder, rootUUID: folder.id, name: "Specs", account: a.account)
    #expect(a.rooms.canInviteByEmail(room))
    #expect(try await a.rooms.invite(email: "BOB@example.com", role: .editor, to: room) == 1)
    let member = try #require(room.info?.member(bob.chainID))
    #expect(member.pending && member.name == "Bob Example")

    await b.rooms.refreshInvitations("")
    let invitation = try #require(b.rooms.invitations.first)
    #expect(invitation.from == "Alice Example" && invitation.kind == .folder)
    let joined = try await b.rooms.accept(invitation, space: home)
    #expect(joined.role == .editor)
    #expect(b.store.node(id: folder.id)?.children.first?.url == "https://specs.example/")
    #expect(b.rooms.invitations.isEmpty)
  }
}

/// Not a test: plays "Alice" for a simulator run. With
/// COMPASS_ROOMS_SEED_SERVER=<url> (a running server) a new chain shares a
/// folder "Team Plans" by link and writes the link to
/// COMPASS_ROOMS_SEED_OUT; the app then joins with -CompassRoomsJoin.
@Suite(.enabled(if: ProcessInfo.processInfo.environment["COMPASS_ROOMS_SEED_SERVER"] != nil))
@MainActor
struct RoomsSeedForSimulator {
  @Test func shareAFolder() async throws {
    let environment = ProcessInfo.processInfo.environment
    let server = URL(string: environment["COMPASS_ROOMS_SEED_SERVER"]!)!
    let alice = await RoomDevice.make(chain: try Chain(seed: Chain.generateSeed()), server: server, name: "Alice")
    let work = alice.store.addSpace(name: "Work")
    let folder = alice.store.addFolder(name: "Team Plans", to: work.pinned)!
    alice.store.addTab(url: "https://www.figma.com/", title: "Design review", to: folder)
    alice.store.addTab(url: "https://linear.app/", title: "Roadmap", to: folder)
    alice.store.addTab(url: "https://docs.google.com/", title: "Launch checklist", to: folder)
    try await alice.sync()
    let room = try await alice.rooms.share(kind: .folder, rootUUID: folder.id, name: folder.title, account: alice.account)
    let link = try await alice.rooms.inviteLink(room, role: .editor, expires: Date().addingTimeInterval(86400), uses: nil)
    try link.write(toFile: environment["COMPASS_ROOMS_SEED_OUT"] ?? "/dev/stdout", atomically: true, encoding: .utf8)
  }
}
