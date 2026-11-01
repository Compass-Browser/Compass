// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import SyncProto
import Testing

struct PasswordsTests {
  // Chromium's PasswordSyncBridgeTest.ShouldComputeClientTagHash.
  @Test func clientTagMatchesChromium() {
    var d = SyncPb_PasswordSpecificsData()
    d.origin = "http://www.origin.com"
    d.usernameElement = "username_element"
    d.usernameValue = "username_value"
    d.passwordElement = "password_element"
    d.signonRealm = "signon_realm"
    #expect(PasswordsBridge.clientTag(d)
      == "http%3A//www.origin.com/|username_element|username_value|password_element|signon_realm")
  }

  // base/strings/escape_unittest.cc, EscapeTest.EscapePath.
  @Test func escapePathMatchesChromium() {
    let input = "\u{02}\n\u{1d} !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~\u{7f}"
    #expect(PasswordsBridge.escapePath(input)
      == "%02%0A%1D%20!%22%23$%25&'()*+,-./0123456789%3A;%3C=%3E%3F@ABCDEFGHIJKLMNOPQRSTUVWXYZ%5B%5C%5D%5E_%60abcdefghijklmnopqrstuvwxyz%7B%7C%7D~%7F")
    #expect(PasswordsBridge.escapePath("é") == "%C3%A9")
  }

  @Test func nestedEncryptionNeverSendsPlaintext() throws {
    let (_, c) = Cryptographer.makeCustomPassphraseNigori(passphrase: "p")
    var s = SyncPb_EntitySpecifics()
    s.password.clientOnlyEncryptedData.signonRealm = "https://example.com/"
    s.password.clientOnlyEncryptedData.passwordValue = "hunter2"
    s.password.clientOnlyEncryptedData.notes.note = [.init()]
    let wire = c.encrypt(password: s)
    #expect(!wire.password.hasClientOnlyEncryptedData && !wire.hasEncrypted)
    #expect(wire.password.hasEncrypted && wire.password.hasEncryptedNotesBackup)
    let bytes = try wire.serializedBytes() as Data
    #expect(bytes.range(of: Data("hunter2".utf8)) == nil)
    #expect(c.decrypt(password: wire)?.password.clientOnlyEncryptedData.passwordValue == "hunter2")
  }

  @Test func matchesRealmsAndSubdomains() {
    func p(_ realm: String, blocked: Bool = false) -> SavedPassword {
      var d = SyncPb_PasswordSpecificsData()
      d.signonRealm = realm
      d.usernameValue = realm
      d.blacklisted = blocked
      return SavedPassword(data: d)
    }
    let all = [p("https://example.com/"), p("https://accounts.google.com/"), p("https://bad.example/", blocked: true)]
    #expect(PasswordsBridge.matches(all, for: URL(string: "https://example.com/login")!).count == 1)
    #expect(PasswordsBridge.matches(all, for: URL(string: "https://www.example.com/")!).count == 1)
    #expect(PasswordsBridge.matches(all, for: URL(string: "https://google.com/")!).count == 1)
    #expect(PasswordsBridge.matches(all, for: URL(string: "https://bad.example/")!).isEmpty)
    #expect(PasswordsBridge.isBlocked(all, for: URL(string: "https://bad.example/x")!))
  }
}

@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
struct PasswordsServerTests {
  let chain = try! Chain(seed: Chain.generateSeed())

  func device(_ server: URL, first: Bool) async -> (SyncEngine, PasswordsBridge, MemoryPasswordStore) {
    let metadata = SyncMetadata()
    let engine = SyncEngine(
      connection: HTTPSyncConnection(serverURL: server, cacheGUID: metadata.cacheGUID, authKey: chain.authKey),
      passphrase: chain.passphrase, store: MemorySyncMetadataStore(metadata), createChainIfEmpty: first)
    let store = MemoryPasswordStore()
    let bridge = PasswordsBridge(store: store, engine: engine)
    await engine.register(bridge)
    return (engine, bridge, store)
  }

  @Test func passwordsRoundTrip() async throws {
    let server = try await TestServer()
    let (macEngine, mac, macStore) = await device(server.url, first: true)
    try await macEngine.sync()
    await mac.save(
      origin: "https://example.com/login", signonRealm: "https://example.com/", username: "matteo",
      password: "hunter2", usernameElement: "user", passwordElement: "pass")
    try await macEngine.sync()

    let (phoneEngine, phone, phoneStore) = await device(server.url, first: false)
    try await phoneEngine.sync()
    #expect(phoneStore.all().map(\.password) == ["hunter2"])

    // A change on the phone reaches the Mac; a deletion too.
    await phone.save(
      origin: "https://example.com/login", signonRealm: "https://example.com/", username: "matteo",
      password: "correct horse", usernameElement: "user", passwordElement: "pass")
    try await phoneEngine.sync()
    try await macEngine.sync()
    #expect(macStore.all().map(\.password) == ["correct horse"])
    await mac.delete(macStore.all()[0])
    try await macEngine.sync()
    try await phoneEngine.sync()
    #expect(phoneStore.all().isEmpty)

    // Nothing readable on the server.
    let db = try Data(contentsOf: server.directory.appendingPathComponent("sync.db"))
    let wal = (try? Data(contentsOf: server.directory.appendingPathComponent("sync.db-wal"))) ?? Data()
    #expect((db + wal).range(of: Data("hunter2".utf8)) == nil)
    #expect((db + wal).range(of: Data("example.com".utf8)) == nil)
  }
}
