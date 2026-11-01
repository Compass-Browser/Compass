// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassModel
@testable import CompassSync
import Foundation
import SyncProto
import Testing

/// A device with several profiles on one chain (docs/design/ios.md, "M8"):
/// one SpaceStore and SpaceDirectory, and per profile account an engine,
/// a spaces bridge, history and passwords. "" is the home account.
@MainActor
final class ProfilesDevice {
  struct Account {
    let engine: SyncEngine
    let spaces: CompassSpacesBridge
    let visits: HistoryBridge
    let directives: HistoryDeleteDirectivesBridge
    let passwords: PasswordsBridge
    let passwordStore: MemoryPasswordStore
  }

  let store = SpaceStore()
  let directory: SpaceDirectory
  let history = try! HistoryStore()
  let chain: Chain
  let server: URL
  let first: Bool
  private(set) var accounts: [String: Account] = [:]

  init(server: URL, chain: Chain, first: Bool) async {
    self.server = server
    self.chain = chain
    self.first = first
    directory = SpaceDirectory(store: store)
    await addAccount("")
  }

  /// Starts a profile's account (empty: the home's).
  func addAccount(_ profile: String) async {
    guard accounts[profile] == nil else { return }
    let metadata = SyncMetadata()
    let engine = SyncEngine(
      connection: HTTPSyncConnection(
        serverURL: server, cacheGUID: metadata.cacheGUID, authKey: chain.authKey(profileUUID: profile)),
      passphrase: chain.passphrase(profileUUID: profile), store: MemorySyncMetadataStore(metadata),
      createChainIfEmpty: first)
    let passwordStore = MemoryPasswordStore()
    let account = Account(
      engine: engine,
      spaces: CompassSpacesBridge(directory: directory, profile: profile, engine: engine, mirrorURL: nil),
      visits: HistoryBridge(history: history, engine: engine, profile: profile),
      directives: HistoryDeleteDirectivesBridge(history: history, engine: engine, profile: profile),
      passwords: PasswordsBridge(store: passwordStore, engine: engine), passwordStore: passwordStore)
    for bridge: any DataTypeBridge in [account.spaces, account.visits, account.directives, account.passwords] {
      await engine.register(bridge)
    }
    accounts[profile] = account
  }

  /// Starts the account of every chain profile the home account lists, as
  /// the app does.
  func addListedProfiles() async {
    for p in directory.profiles where p.uuid != store.homeProfileUUID { await addAccount(p.uuid) }
  }

  /// One cycle per account, the home's first unless `order` says otherwise.
  func sync(order: [String]? = nil) async throws {
    for profile in order ?? ([""] + accounts.keys.filter { !$0.isEmpty }.sorted()) {
      guard let a = accounts[profile] else { continue }
      await a.spaces.flush()
      try await a.engine.sync()
      await a.spaces.flush()
      if await a.engine.hasPendingChanges { try await a.engine.sync() }
    }
  }

  func syncUntilQuiet() async throws {
    for _ in 0..<3 {
      try await sync()
      var pending = false
      for a in accounts.values where await a.engine.hasPendingChanges { pending = true }
      if !pending { return }
    }
  }

  /// "The Mac" writes the chain's profile list into the home account.
  func publishProfiles(_ profiles: [ChainProfile]) async {
    for p in profiles {
      var e = SyncPb_EntitySpecifics()
      e.compassSpaces.profile.uuid = p.uuid
      e.compassSpaces.profile.name = p.name
      if p.home { e.compassSpaces.profile.home = true }
      await accounts[""]!.engine.put(.compassSpaces, tag: p.uuid, specifics: e)
    }
  }

  func space(_ name: String) -> Space { store.spaces.first { $0.name == name }! }

  /// Spaces (with profile) and their entries, for comparing two devices.
  var outline: String {
    func line(_ n: SidebarNode, _ depth: Int) -> [String] {
      [String(repeating: "  ", count: depth) + "\(n.displayTitle) \(n.url)"] + n.children.flatMap { line($0, depth + 1) }
    }
    var out: [String] = []
    for root in store.favoritesRoots where !root.children.isEmpty {
      out.append("favorites of \(store.profile(of: root))")
      out += root.children.flatMap { line($0, 1) }
    }
    for s in store.spaces {
      out.append("space \(s.name) profile \(s.profileUUID)")
      for root in [s.pinned, s.today] { out += root.children.flatMap { line($0, 1) } }
    }
    return out.joined(separator: "\n")
  }
}

@Suite(.enabled(if: TestServer.available, "set COMPASS_SYNC_SERVER to the server binary"), .serialized)
@MainActor
struct ProfilesServerTests {
  let chain = try! Chain(seed: Chain.generateSeed())
  let home = "2b5c3e1a-0000-4000-8000-00000000000a"
  let work = "2b5c3e1a-0000-4000-8000-00000000000b"

  /// The Mac: Home profile with "Personal" and "Side", Work with "Work",
  /// in the switcher as Personal, Work, Side; favorites in both.
  func makeMac(_ server: TestServer) async throws -> ProfilesDevice {
    let mac = await ProfilesDevice(server: server.url, chain: chain, first: true)
    await mac.addAccount(work)
    mac.store.homeProfileUUID = home
    try await mac.sync()
    await mac.publishProfiles([
      ChainProfile(uuid: home, name: "Personal", home: true), ChainProfile(uuid: work, name: "Work"),
    ])
    let personal = mac.store.addSpace(name: "Personal", theme: SpaceTheme(.green), profile: home)
    mac.store.addTab(url: "https://news.example/", title: "News", to: personal.pinned)
    let w = mac.store.addSpace(name: "Work", theme: SpaceTheme(.purple), profile: work)
    let folder = mac.store.addFolder(name: "Projects", to: w.pinned)!
    mac.store.addTab(url: "https://git.example/", title: "Git", to: folder)
    mac.store.addTab(url: "https://chat.example/", title: "Chat", to: w.today)
    let side = mac.store.addSpace(name: "Side", theme: SpaceTheme(.orange), profile: home)
    mac.store.addTab(url: "https://side.example/", title: "Side project", to: side.pinned)
    mac.store.addTab(url: "https://mail.example/", title: "Mail", to: mac.store.favorites(forProfile: home))
    mac.store.addTab(url: "https://jira.example/", title: "Jira", to: mac.store.favorites(forProfile: work))
    try await mac.syncUntilQuiet()
    return mac
  }

  /// A phone joining by recovery code: the home account first, then the
  /// accounts its profile list names.
  func joinPhone(_ server: TestServer) async throws -> ProfilesDevice {
    let phone = await ProfilesDevice(server: server.url, chain: chain, first: false)
    try await phone.sync()
    #expect(phone.directory.profiles.map(\.name) == ["Personal", "Work"])
    #expect(phone.store.homeProfileUUID == home)
    await phone.addListedProfiles()
    try await phone.syncUntilQuiet()
    return phone
  }

  @Test func everyProfilesSpacesInOneOrder() async throws {
    let server = try await TestServer()
    let mac = try await makeMac(server)
    let phone = try await joinPhone(server)

    #expect(phone.store.spaces.map(\.name) == ["Personal", "Work", "Side"])
    #expect(phone.space("Work").profileUUID == work)
    #expect(phone.space("Side").profileUUID == home)
    #expect(phone.outline == mac.outline, "\n\(phone.outline)\n---\n\(mac.outline)")
    #expect(phone.store.favorites(for: phone.space("Work")).children.map(\.title) == ["Jira"])
    #expect(phone.store.favorites(for: phone.space("Side")).children.map(\.title) == ["Mail"])
    // The Mac's home account holds no Work entity and vice versa.
    let macWork = mac.accounts[work]!.engine
    let macHome = mac.accounts[""]!.engine
    #expect(await macWork.knownTags(.compassSpaces).contains(phone.space("Work").id))
    #expect(await !macHome.knownTags(.compassSpaces).contains(phone.space("Work").id))

    // Nothing to commit afterwards on either side (no echo).
    try await phone.sync()
    for a in phone.accounts.values { #expect(await a.engine.hasPendingChanges == false) }
  }

  @Test func phoneEditsGoToTheirProfilesAccount() async throws {
    let server = try await TestServer()
    let mac = try await makeMac(server)
    let phone = try await joinPhone(server)

    let phoneWork = phone.space("Work")
    phone.store.renameSpace(phoneWork, to: "Job")
    let tab = phone.store.addTab(url: "https://ci.example/", title: "CI", to: phoneWork.pinned)!
    let fav = phone.store.addTab(url: "https://wiki.example/", title: "Wiki", to: phone.store.favorites(for: phoneWork))!
    let personalTab = phone.store.addTab(url: "https://bank.example/", title: "Bank", to: phone.space("Personal").pinned)!
    // A space made from a Work space is Work's; Side moves between
    // Personal and Job (positions across accounts).
    let made = phone.store.addSpace(name: "Clients", profile: phoneWork.profileUUID)
    phone.store.moveSpace(phone.space("Side"), to: 1)
    try await phone.syncUntilQuiet()

    let phoneWorkEngine = phone.accounts[work]!.engine
    let phoneHomeEngine = phone.accounts[""]!.engine
    for id in [phoneWork.id, tab.id, fav.id, made.id] {
      #expect(await phoneWorkEngine.knownTags(.compassSpaces).contains(id))
      #expect(await !phoneHomeEngine.knownTags(.compassSpaces).contains(id))
    }
    #expect(await phoneHomeEngine.knownTags(.compassSpaces).contains(personalTab.id))
    #expect(await !phoneWorkEngine.knownTags(.compassSpaces).contains(personalTab.id))

    try await mac.syncUntilQuiet()
    #expect(mac.store.spaces.map(\.name) == ["Personal", "Side", "Job", "Clients"])
    #expect(mac.space("Clients").profileUUID == work)
    #expect(mac.outline == phone.outline, "\n\(mac.outline)\n---\n\(phone.outline)")
  }

  /// A space moved Work → Home on the Mac: its Work account deletes it, the
  /// Home account creates it under the same IDs. The phone gets the two in
  /// either order.
  @Test(arguments: [true, false])
  func spaceMovedBetweenProfiles(createFirst: Bool) async throws {
    let server = try await TestServer()
    let mac = try await makeMac(server)
    let phone = try await joinPhone(server)

    let macWork = mac.space("Work")
    let ids = Set([macWork.id] + macWork.allNodes.map(\.id))
    mac.store.setProfile(macWork, to: home)
    try await mac.syncUntilQuiet()
    #expect(await mac.accounts[work]!.engine.knownTags(.compassSpaces).isDisjoint(with: ids))
    #expect(await mac.accounts[""]!.engine.knownTags(.compassSpaces).isSuperset(of: ids))

    // Create first: the home account (it adopts the space with its
    // entries). Delete first: the Work account (it goes, then comes back).
    try await phone.sync(order: createFirst ? ["", work] : [work, ""])
    if !createFirst {
      try await phone.sync(order: [""])
    }
    try await phone.syncUntilQuiet()
    #expect(phone.store.spaces.map(\.name) == ["Personal", "Work", "Side"])
    #expect(phone.space("Work").profileUUID == home)
    #expect(Set([phone.space("Work").id] + phone.space("Work").allNodes.map(\.id)) == ids)
    #expect(await phone.accounts[work]!.engine.knownTags(.compassSpaces).isDisjoint(with: ids))
    #expect(await phone.accounts[""]!.engine.knownTags(.compassSpaces).isSuperset(of: ids))
    #expect(phone.outline == mac.outline, "\n\(phone.outline)\n---\n\(mac.outline)")

    // And back, from the phone this time.
    phone.store.setProfile(phone.space("Work"), to: work)
    try await phone.syncUntilQuiet()
    try await mac.syncUntilQuiet()
    #expect(mac.space("Work").profileUUID == work)
    #expect(await mac.accounts[""]!.engine.knownTags(.compassSpaces).isDisjoint(with: ids))
    #expect(mac.outline == phone.outline, "\n\(mac.outline)\n---\n\(phone.outline)")
  }

  @Test func passwordsAndHistoryStayInTheirProfile() async throws {
    let server = try await TestServer()
    let mac = try await makeMac(server)
    await mac.accounts[work]!.passwords.save(
      origin: "https://jira.example/login", signonRealm: "https://jira.example/", username: "matteo",
      password: "work-secret")
    try await mac.syncUntilQuiet()
    let phone = try await joinPhone(server)
    #expect(phone.accounts[work]!.passwordStore.all().map(\.password) == ["work-secret"])
    #expect(phone.accounts[""]!.passwordStore.all().isEmpty)

    // Visits go to the account of the entry's profile.
    await phone.accounts[work]!.visits.record(url: "https://jira.example/browse/1", title: "Ticket")
    await phone.accounts[""]!.visits.record(url: "https://news.example/today", title: "Today")
    try await phone.syncUntilQuiet()
    try await mac.syncUntilQuiet()
    #expect(mac.history.search("ticket").map(\.url) == ["https://jira.example/browse/1"])
    #expect(mac.history.count(profile: work) == 1)
    #expect(mac.history.count(profile: "") == 1)
    // A directive in Work clears Work's history only.
    await mac.accounts[work]!.directives.deleteHistory(from: Date(timeIntervalSince1970: 0), to: Date())
    try await mac.syncUntilQuiet()
    try await phone.syncUntilQuiet()
    #expect(phone.history.count(profile: work) == 0)
    #expect(phone.history.count(profile: "") == 1)

    // Nothing readable on the server: names, URLs, passwords.
    let db = try Data(contentsOf: server.directory.appendingPathComponent("sync.db"))
    let wal = (try? Data(contentsOf: server.directory.appendingPathComponent("sync.db-wal"))) ?? Data()
    for text in ["work-secret", "jira.example", "git.example", "Projects", "Personal", "news.example", home, work] {
      #expect((db + wal).range(of: Data(text.utf8)) == nil, "\(text) on the server")
    }
  }
}
