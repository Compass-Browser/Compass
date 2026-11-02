// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
@testable import CompassSync
import Foundation
import SyncProto
import Testing

private func specifics(_ id: String, name: String, updateURL: String = "https://clients2.google.com/service/update2/crx")
  -> SyncPb_EntitySpecifics
{
  var e = SyncPb_ExtensionSpecifics()
  e.id = id
  e.name = name
  e.version = "1.2.3"
  e.enabled = true
  e.updateURL = updateURL
  var s = SyncPb_EntitySpecifics()
  s.extension = e
  return s
}

private func change(_ s: SyncPb_EntitySpecifics?, tag: String) -> RemoteChange {
  RemoteChange(
    clientTagHash: ClientTagHash.make(type: .extensions, tag: tag), specifics: s, serverID: tag, version: 1,
    mtime: 0, ctime: 0, originatorCacheGUID: "mac")
}

struct ExtensionsBridgeTests {
  let darkReader = "eimadpbcbfnmbkopoojfekhnkhdbieeh"
  let onePassword = "aeblfdkhhhdcdjpifhhbdiojplfjncoa"

  @Test func appliesMirrorsAndDeletes() async throws {
    let url = FileManager.default.temporaryDirectory.appendingPathComponent("ext-\(UUID().uuidString).json")
    defer { try? FileManager.default.removeItem(at: url) }
    let bridge = ExtensionsBridge(stateURL: url)
    #expect(bridge.clientTag(for: specifics(darkReader, name: "Dark Reader")) == darkReader)
    await bridge.apply([
      change(specifics(onePassword, name: "1Password"), tag: onePassword),
      change(specifics(darkReader, name: "Dark Reader"), tag: darkReader),
      change(specifics("abcdefghijklmnopabcdefghijklmnop", name: "Self-hosted", updateURL: "https://example.com/u.xml"),
             tag: "abcdefghijklmnopabcdefghijklmnop"),
    ])
    #expect(bridge.extensions.map(\.name) == ["1Password", "Dark Reader", "Self-hosted"])
    #expect(bridge.extensions.filter(\.isFromWebStore).count == 2)
    // The mirror brings the list back without a download.
    #expect(ExtensionsBridge(stateURL: url).extensions.count == 3)

    await bridge.apply([change(nil, tag: darkReader)])
    #expect(bridge.extensions.map(\.id) == [onePassword, "abcdefghijklmnopabcdefghijklmnop"])
    #expect(ExtensionsBridge(stateURL: url).extensions.count == 2)
  }
}

@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
struct ExtensionsServerTests {
  let chain = try! Chain(seed: Chain.generateSeed())

  // The desktop's EXTENSIONS entities (written as Chromium would, encrypted
  // with the chain's passphrase) reach the phone's bridge.
  @Test func desktopListReachesThePhone() async throws {
    let server = try await TestServer()
    func engine(first: Bool) -> SyncEngine {
      let metadata = SyncMetadata()
      return SyncEngine(
        connection: HTTPSyncConnection(serverURL: server.url, cacheGUID: metadata.cacheGUID, authKey: chain.authKey),
        passphrase: chain.passphrase, store: MemorySyncMetadataStore(metadata), createChainIfEmpty: first)
    }
    let mac = engine(first: true)
    await mac.register(ExtensionsBridge(stateURL: nil))
    try await mac.sync()
    await mac.put(.extensions, tag: darkReaderID, specifics: specifics(darkReaderID, name: "Dark Reader"))
    try await mac.sync()

    let phone = engine(first: false)
    let bridge = ExtensionsBridge(stateURL: nil)
    await phone.register(bridge)
    try await phone.sync()
    #expect(bridge.extensions.map(\.id) == [darkReaderID], "\(server.log)")
    #expect(bridge.extensions.first?.version == "1.2.3")
  }

  let darkReaderID = "eimadpbcbfnmbkopoojfekhnkhdbieeh"
}
