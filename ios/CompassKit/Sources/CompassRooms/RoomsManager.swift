// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import CompassEasels
import CompassModel
import CompassSync
import Foundation
import Observation
import SyncProto

/// Sharing on this device (docs/design/ios.md, "Rooms"): every room the
/// chain's accounts are members of, from the mounts their COMPASS_SPACES
/// carries, each synced by a `RoomSync`; and the actions of the share
/// sheet and the join preview. The bridges ask it what's shared
/// (`RoomBacking`) and leave that out of the chain.
@MainActor
@Observable
public final class RoomsManager: RoomBacking {
  /// One server account of the chain (the home's, or a profile's).
  public final class Account {
    public let profile: String
    public weak var bridge: CompassSpacesBridge?
    public let serverURL: URL
    public let authKey: Data
    public let identity: IdentityKey
    public let cacheGUID: String
    public var chainID: String { Ed25519.publicKey(fromSeed: authKey).hexString(uppercase: false) }
    var identityRegistered: Set<String> = []

    public init(
      profile: String, bridge: CompassSpacesBridge?, serverURL: URL, authKey: Data, identity: IdentityKey,
      cacheGUID: String
    ) {
      self.profile = profile
      self.bridge = bridge
      self.serverURL = serverURL
      self.authKey = authKey
      self.identity = identity
      self.cacheGUID = cacheGUID
    }
  }

  /// "You were removed from …": the banner, until dismissed.
  public struct Notice: Identifiable, Equatable, Sendable {
    public let id = UUID()
    public var message: String
  }

  /// An email invitation waiting for an answer.
  public struct Invitation: Identifiable, Equatable, Sendable {
    public var roomID: String
    public var kind: RoomKind
    public var from: String
    public var account: String
    public var id: String { roomID }
  }

  public let store: SpaceStore
  public let easels: EaselStore?
  public let directory: SpaceDirectory
  let stateDirectory: URL?
  let session: URLSession?
  @ObservationIgnored public private(set) var accounts: [String: Account] = [:]
  /// By room ID.
  public private(set) var rooms: [String: RoomSync] = [:]
  /// The account (profile) each room's mount is in.
  @ObservationIgnored var roomAccount: [String: String] = [:]
  public var notices: [Notice] = []
  public private(set) var invitations: [Invitation] = []
  /// "Your Name for Sharing": the name members see when the server has no
  /// company sign-in.
  public var displayName: String
  /// Bumped when rooms change (members, roles), for views.
  public private(set) var revision = 0
  /// The sync server's company sign-in (email invites), once asked.
  public private(set) var loginProvider: [String: String?] = [:]
  /// Cycles run while active: long polls per room.
  @ObservationIgnored private var loops: [String: Task<Void, Never>] = [:]
  @ObservationIgnored private var kicks: [String: Task<Void, Never>] = [:]
  @ObservationIgnored public private(set) var active = false

  public init(
    directory: SpaceDirectory, easels: EaselStore?, stateDirectory: URL?, displayName: String = "",
    session: URLSession? = nil
  ) {
    self.directory = directory
    store = directory.store
    self.easels = easels
    self.stateDirectory = stateDirectory
    self.displayName = displayName
    self.session = session
    if let stateDirectory {
      try? FileManager.default.createDirectory(at: stateDirectory, withIntermediateDirectories: true)
    }
    directory.rooms = self
    store.localChangeObservers.append { [weak self] change in self?.localChanged(change) }
    easels?.changeObservers.append { [weak self] in self?.easelsChanged() }
  }

  // MARK: Accounts

  /// An account started: its mounts' rooms start too.
  public func addAccount(_ account: Account) {
    accounts[account.profile] = account
    if let bridge = account.bridge { mountsChanged(bridge) }
  }

  /// Stops everything (sync left): local copies stay, unshared.
  public func stop() {
    setActive(false)
    for room in rooms.values { room.save() }
    rooms = [:]
    roomAccount = [:]
    accounts = [:]
    invitations = []
  }

  func account(of room: RoomSync) -> Account? { roomAccount[room.roomID].flatMap { accounts[$0] } }

  func client(_ account: Account, server: String? = nil) -> RoomClient {
    let url = server.flatMap(URL.init(string:)) ?? account.serverURL
    let same = url.absoluteString == account.serverURL.absoluteString
    return RoomClient(serverURL: url, authKey: account.authKey, cacheGUID: same ? account.cacheGUID : nil, session: session)
  }

  func client(for room: RoomSync) -> RoomClient? {
    account(of: room).map { client($0, server: room.mount.serverURL) }
  }

  /// The account a space's items share from: its owner's.
  public func account(for space: Space?) -> Account? {
    if let space, let bridge = directory.owner(of: space), let account = accounts[bridge.accountProfile] {
      return account
    }
    return accounts[""] ?? accounts.values.first
  }

  /// Registers the account's identity key on a server (once a run).
  func registerIdentity(_ account: Account, client: RoomClient) async throws {
    let key = client.serverURL.absoluteString
    guard !account.identityRegistered.contains(key) else { return }
    try await client.putIdentity(account.identity.publicKey)
    account.identityRegistered.insert(key)
  }

  /// At start: the identity key on the account's server (so email invites
  /// find it), the server's sign-in, and pending invitations.
  public func checkServer(_ profile: String) async {
    guard let account = accounts[profile] else { return }
    let client = client(account)
    try? await registerIdentity(account, client: client)
    if let provider = try? await client.loginProvider() {
      loginProvider[account.serverURL.absoluteString] = .some(provider)
    } else {
      loginProvider[account.serverURL.absoluteString] = .some(nil)
    }
    await refreshInvitations(profile)
  }

  /// Whether email invites work for a room (its server has sign-in).
  public func canInviteByEmail(_ room: RoomSync) -> Bool {
    guard let server = URL(string: room.mount.serverURL) else { return false }
    return (loginProvider[server.absoluteString] ?? nil) != nil
  }

  // MARK: RoomBacking

  public func isRoomSpace(_ id: String) -> Bool {
    rooms.values.contains { $0.kind == .space && $0.rootUUID == id }
  }

  public func isRoomNode(_ node: SidebarNode) -> Bool { room(of: node) != nil }

  public func isRoomNodeID(_ id: String) -> Bool { rooms.values.contains { $0.containsNode(id: id) } }

  public func isRoomEasel(_ id: String) -> Bool {
    for room in rooms.values {
      if room.easelIDs.contains(id) { return true }
      if case .easelObject(let o)? = room.mirror[id]?.entity, room.easelIDs.contains(o.easelUuid) { return true }
      if let object = easels?.object(id), room.easelIDs.contains(object.easelUuid) { return true }
      if case .easelAsset? = room.mirror[id]?.entity { return !personalAssetIDs.contains(id) }
    }
    return false
  }

  /// Images a board that isn't shared shows (they stay in the chain too).
  var personalAssetIDs: Set<String> {
    guard let easels else { return [] }
    let shared = Set(rooms.values.flatMap(\.easelIDs))
    var out: Set<String> = []
    for board in easels.easels where !shared.contains(board.uuid) {
      for object in easels.objects(of: board.uuid) {
        for sha in Easels.assets(of: object) { out.insert(Easels.assetUUID(sha256: sha)) }
      }
    }
    return out
  }

  /// The mounts of an account changed: rooms start, end or update.
  public func mountsChanged(_ bridge: CompassSpacesBridge) {
    let profile = bridge.accountProfile
    let mounts = bridge.roomMounts().filter(\.isUsable)
    let ids = Set(mounts.map(\.roomID))
    var ended: [RoomSync] = []
    for (id, room) in rooms where roomAccount[id] == profile && !ids.contains(id) { ended.append(room) }
    for room in ended { end(room, notice: nil, deleteMount: false) }
    for mount in mounts {
      if let room = rooms[mount.roomID] {
        guard room.mount != mount else { continue }
        // Keys and placement from another device; the root once known.
        var merged = mount
        for (epoch, key) in room.mount.keyByEpoch where merged.keyByEpoch[epoch] == nil { merged.setKey(key, epoch: epoch) }
        if merged.rootUuid.isEmpty { merged.rootUuid = room.mount.rootUuid }
        room.mount = merged
        room.apply(changed: room.mirror, deleted: [])
      } else {
        start(mount, profile: profile)
      }
    }
    revision += 1
  }

  @discardableResult
  func start(_ mount: RoomMount, profile: String) -> RoomSync {
    let room = RoomSync(
      mount: mount, store: store, easels: easels,
      stateURL: stateDirectory?.appendingPathComponent("\(mount.roomID).json"))
    room.spaceProfile = { [weak self] in
      guard let self, !profile.isEmpty else { return self?.store.homeProfileUUID ?? "" }
      return profile
    }
    room.fallbackSpace = { [weak self] in
      guard let self else { return nil }
      return self.store.spaces.first { self.directory.owner(of: $0)?.accountProfile == profile } ?? self.store.spaces.first
    }
    room.onMountChanged = { [weak self, weak room] mount in
      guard let self, let room else { return }
      self.accounts[profile]?.bridge?.putRoomMount(mount)
      _ = room
      self.revision += 1
    }
    rooms[mount.roomID] = room
    roomAccount[mount.roomID] = profile
    // What came before (saved), at once.
    if !room.mirror.isEmpty { room.apply(changed: room.mirror, deleted: []) }
    if active { startLoop(room) }
    return room
  }

  /// The room's over here (left, removed, deleted): the local copy stays,
  /// and syncs in the account as anything else.
  func end(_ room: RoomSync, notice: String?, deleteMount: Bool) {
    loops.removeValue(forKey: room.roomID)?.cancel()
    rooms[room.roomID] = nil
    let profile = roomAccount.removeValue(forKey: room.roomID)
    if let stateURL = room.stateURL { try? FileManager.default.removeItem(at: stateURL) }
    let bridge = profile.flatMap { accounts[$0]?.bridge }
    if deleteMount { bridge?.deleteRoomMount(roomID: room.roomID) }
    for bridge in directory.bridges { bridge.roomsChanged() }
    if let notice { notices.append(Notice(message: notice)) }
    revision += 1
  }

  // MARK: Lookup

  /// The room a node is in.
  public func room(of node: SidebarNode) -> RoomSync? { rooms.values.first { $0.contains(node) } }
  /// The room of a shared space.
  public func room(ofSpace id: String) -> RoomSync? { rooms.values.first { $0.kind == .space && $0.rootUUID == id } }
  /// The room of a shared board (the easel itself, or one a shared folder
  /// or space shows).
  public func room(ofEasel id: String) -> RoomSync? { rooms.values.first { $0.easelIDs.contains(id) } }
  /// The room whose shared item is exactly this node (a shared folder, a
  /// shared easel's tab).
  public func room(rootedAt node: SidebarNode) -> RoomSync? {
    rooms.values.first {
      ($0.kind == .folder && $0.rootUUID == node.id) || ($0.kind == .easel && $0.easelTabID == node.id)
    }
  }

  /// Whether a node may be edited here (not a viewer's).
  public func canEdit(_ node: SidebarNode) -> Bool { room(of: node)?.role.canEdit ?? true }
  public func canEdit(easel id: String) -> Bool { room(ofEasel: id)?.role.canEdit ?? true }

  // MARK: Local changes

  func localChanged(_ change: SpaceStoreChange) {
    guard !rooms.isEmpty else { return }
    let ids = change.putNodes.union(change.deletedNodes).union(change.putSpaces).union(change.deletedSpaces)
    for room in rooms.values {
      let touched = ids.contains { id in
        id == room.rootUUID || room.mirror[id] != nil || store.node(id: id).map(room.contains) == true
      }
      guard touched else { continue }
      if !room.role.canEdit {
        room.revertLocalEdits()
        continue
      }
      if !room.rootIsHere { room.restoreIfMissing() }
      updatePlacement(room)
      kick(room)
    }
  }

  func easelsChanged() {
    for room in rooms.values where !room.easelIDs.isEmpty {
      if !room.role.canEdit {
        if !room.pendingWrites().isEmpty { room.revertLocalEdits() }
        continue
      }
      if !room.pendingWrites().isEmpty { kick(room) }
    }
  }

  /// A shared folder (or easel tab) moved here: its placement is the
  /// mount's, for every device of the account.
  func updatePlacement(_ room: RoomSync) {
    let node: SidebarNode? =
      switch room.kind {
      case .folder: store.node(id: room.rootUUID)
      case .easel: store.node(id: room.easelTabID)
      case .space: nil
      }
    guard let node, let parent = node.parent, let space = store.space(of: node) else { return }
    var mount = room.mount
    mount.spaceUuid = space.id
    if parent.isRoot { mount.clearParentUuid() } else { mount.parentUuid = parent.id }
    mount.position = CompassSpacesBridge.proto(node.position)
    guard mount != room.mount else { return }
    room.mount = mount
    accounts[roomAccount[room.roomID] ?? ""]?.bridge?.putRoomMount(mount)
  }

  // MARK: Cycles

  /// One cycle of a room: local edits up, then what's new down.
  public func sync(_ room: RoomSync, wait: Int = 0) async {
    guard let account = account(of: room), let client = client(for: room) else { return }
    do {
      if room.info == nil { _ = try await room.refresh(client, identity: account.identity) }
      if room.info?.pending == true { return }
      try await room.push(client, identity: account.identity)
      try await room.pull(client, identity: account.identity, wait: wait)
      if !room.role.canEdit { room.revertLocalEdits() }
      try await room.push(client, identity: account.identity)
    } catch RoomError.removed {
      end(
        room,
        notice: String(localized: "You were removed from “\(room.name)”. Your copy stays on this device.", bundle: .module, comment: "Sidebar banner: the user was removed from a shared space, folder or Easel. The argument is its name."),
        deleteMount: true)
    } catch RoomError.notFound {
      end(
        room,
        notice: String(localized: "“\(room.name)” is no longer shared. Your copy stays on this device.", bundle: .module, comment: "Sidebar banner: a shared space, folder or Easel was deleted by its owner. The argument is its name."),
        deleteMount: true)
    } catch {}
    revision += 1
  }

  /// Every room once (tests, and after a sync cycle).
  public func syncAll() async {
    for room in rooms.values.sorted(by: { $0.roomID < $1.roomID }) where rooms[room.roomID] === room {
      await sync(room)
    }
  }

  /// A room's members or keys changed (a `room` event).
  public func handle(_ event: SyncNotifier.RoomEvent, account profile: String) {
    guard let room = rooms[event.roomID] else {
      Task { await refreshInvitations(profile) }
      return
    }
    if event.changes.contains("deleted") {
      Task { await sync(room) }
      return
    }
    if event.changes.contains(where: { $0 == "members" || $0 == "keys" || $0 == "invites" }) {
      room.info = nil
    }
    kick(room)
  }

  /// While the app is active, each room long-polls its changes.
  public func setActive(_ isActive: Bool) {
    active = isActive
    if isActive {
      for room in rooms.values { startLoop(room) }
    } else {
      for task in loops.values { task.cancel() }
      loops = [:]
    }
  }

  func startLoop(_ room: RoomSync) {
    guard active, loops[room.roomID] == nil else { return }
    loops[room.roomID] = Task { [weak self, weak room] in
      var failures = 0
      while !Task.isCancelled {
        guard let self, let room, self.rooms[room.roomID] === room else { return }
        let before = room.state.seq
        await self.sync(room, wait: RoomClient.maxWait)
        if Task.isCancelled { return }
        failures = room.state.seq == before && room.info == nil ? failures + 1 : 0
        if failures > 0 { try? await Task.sleep(for: .seconds(min(60, pow(2, Double(min(failures, 6)))))) }
      }
    }
  }

  /// Local edits go up soon: the long poll starts over.
  func kick(_ room: RoomSync) {
    kicks[room.roomID]?.cancel()
    kicks[room.roomID] = Task { [weak self, weak room] in
      try? await Task.sleep(for: .milliseconds(700))
      guard !Task.isCancelled, let self, let room, self.rooms[room.roomID] === room else { return }
      self.loops.removeValue(forKey: room.roomID)?.cancel()
      if self.active {
        self.startLoop(room)
      } else {
        await self.sync(room)
      }
    }
  }

  // MARK: Sharing

  /// Shares a pinned folder, a space or a board: a new room this account
  /// owns, its records written, the mount in the account.
  @discardableResult
  public func share(kind: RoomKind, rootUUID: String, name: String, account: Account) async throws -> RoomSync {
    let client = client(account)
    try await registerIdentity(account, client: client)
    let key = RoomCrypto.generateRoomKey()
    // The client chooses the room's ID, so the epoch 1 envelope is sealed
    // for it from the start (its HPKE info binds it); a taken ID (409
    // room_exists) gets another.
    var created: RoomInfo?
    for attempt in 0..<3 {
      let roomID = RoomCrypto.generateRoomID()
      let envelope = try RoomCrypto.sealEnvelope(
        recipientPublicKey: account.identity.publicKey, roomID: roomID, epoch: 1, roomKey: key)
      do {
        created = try await client.createRoom(
          id: roomID, kind: kind, identityPub: account.identity.publicKey, envelope: envelope)
        guard created?.id == RoomInviteLink.b64(roomID) else { throw RoomError.badReply }
        break
      } catch RoomError.roomExists where attempt < 2 {
        continue
      }
    }
    guard let info = created else { throw RoomError.roomExists }
    var mount = RoomMount()
    mount.roomID = info.id
    mount.serverURL = account.serverURL.absoluteString
    mount.kind = kind.rawValue
    mount.rootUuid = rootUUID
    mount.role = RoomRole.owner.rawValue
    mount.name = name
    mount.setKey(key, epoch: 1)
    let room = start(mount, profile: account.profile)
    room.info = info
    updatePlacement(room)
    account.bridge?.putRoomMount(room.mount)
    for bridge in directory.bridges { bridge.roomsChanged() }
    try await setProfile(room, account: account)
    try await room.push(client, identity: account.identity)
    revision += 1
    return room
  }

  /// A link anyone who has it can join with, until it expires or is used
  /// up. It carries the current key.
  public func inviteLink(_ room: RoomSync, role: RoomRole, expires: Date?, uses: Int?) async throws -> String {
    guard let account = account(of: room), let client = client(for: room) else { throw RoomError.removed }
    if room.info == nil { _ = try await room.refresh(client, identity: account.identity) }
    let epoch = room.epoch
    guard let key = room.keys[epoch] else { throw RoomError.staleEpoch }
    let secret = RoomCrypto.generateInviteSecret()
    let invite = try await client.createInvite(
      room.roomID, role: role, secretHash: RoomCrypto.inviteSecretHash(secret), expires: expires, uses: uses)
    let link = RoomInviteLink(
      serverURL: room.mount.serverURL, roomID: room.roomIDBytes, inviteID: invite.id, inviteSecret: secret,
      epochKey: key, epoch: epoch, name: String(room.name.utf8.prefix(RoomInviteLink.maxNameLength)) ?? "",
      kind: room.kind)
    guard let text = link.link else { throw RoomError.badArgument("link") }
    room.info = try? await client.room(room.roomID)
    revision += 1
    return text
  }

  public func revokeInvite(_ room: RoomSync, invite: String) async throws {
    guard let client = client(for: room) else { return }
    try await client.revokeInvite(room.roomID, invite: invite)
    room.info = try? await client.room(room.roomID)
    revision += 1
  }

  /// Invites the accounts of an email address (company sign-in servers):
  /// they're added pending, with the keys, and accept in their Compass.
  public func invite(email: String, role: RoomRole, to room: RoomSync) async throws -> Int {
    guard let account = account(of: room), let client = client(for: room) else { throw RoomError.removed }
    let found = try await client.directory(email: email.trimmingCharacters(in: .whitespaces))
    if room.info == nil { _ = try await room.refresh(client, identity: account.identity) }
    let current = Set(room.info?.members.map { $0.clientID.lowercased() } ?? [])
    var invitees: [RoomInvitee] = []
    for target in found where !current.contains(target.clientID.lowercased()) {
      var envelopes: [RoomEnvelope] = []
      for (epoch, key) in room.keys.sorted(by: { $0.key < $1.key }) where epoch <= room.epoch {
        envelopes.append(
          RoomEnvelope(
            epoch: epoch,
            sealed: try RoomCrypto.sealEnvelope(
              recipientPublicKey: target.identityPub, roomID: room.roomIDBytes, epoch: epoch, roomKey: key)))
      }
      invitees.append(RoomInvitee(clientID: target.clientID, role: role, identityPub: target.identityPub, envelopes: envelopes))
    }
    guard !invitees.isEmpty else { return 0 }
    room.info = try await client.inviteMembers(room.roomID, invitees)
    for invitee in invitees { room.state.envelopesSent[invitee.clientID.lowercased()] = room.epoch }
    room.save()
    revision += 1
    return invitees.count
  }

  public func setRole(_ role: RoomRole, member: String, in room: RoomSync) async throws {
    guard let client = client(for: room) else { return }
    try await client.setRole(room.roomID, member: member, role: role)
    room.info = try? await client.room(room.roomID)
    revision += 1
  }

  /// Removes a member and rotates the key, so it can't read what's written
  /// from then on.
  public func remove(member: String, from room: RoomSync) async throws {
    guard let account = account(of: room), let client = client(for: room) else { return }
    try await client.removeMember(room.roomID, member: member)
    try await room.rotate(client, identity: account.identity)
    revision += 1
  }

  /// Leaves the room (a pending member: declines). The copy stays here,
  /// unshared.
  public func leave(_ room: RoomSync) async throws {
    guard let account = account(of: room), let client = client(for: room) else { return }
    do {
      try await client.removeMember(room.roomID, member: account.chainID)
    } catch RoomError.removed, RoomError.notFound {}
    end(room, notice: nil, deleteMount: true)
  }

  /// The owner ends the room for everyone; each keeps a copy.
  public func deleteForEveryone(_ room: RoomSync) async throws {
    guard let client = client(for: room) else { return }
    do {
      try await client.deleteRoom(room.roomID)
    } catch RoomError.notFound {}
    end(room, notice: nil, deleteMount: true)
  }

  /// Members as people: the company name or email, else the name they
  /// chose, else a short ID.
  public func displayName(of member: RoomMember, in room: RoomSync) -> String {
    if !member.name.isEmpty { return member.name }
    if let profile = member.profile,
      let name = RoomProfile.open(profile, chainID: member.clientID, roomID: room.roomIDBytes, keys: room.keys),
      !name.isEmpty
    {
      return name
    }
    if !member.email.isEmpty { return member.email }
    return String(localized: "Member \(String(member.clientID.prefix(6)))", bundle: .module, comment: "Sharing: a member with no name or email; the argument is a short ID.")
  }

  /// Whether a member is this device's account.
  public func isMe(_ member: RoomMember, in room: RoomSync) -> Bool {
    account(of: room)?.chainID == member.clientID.lowercased()
  }

  /// Sets this account's name in a room (its profile).
  func setProfile(_ room: RoomSync, account: Account) async throws {
    let name = displayName.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let client = client(for: room) else { return }
    let epoch = room.epoch
    guard let key = room.keys[epoch] else { return }
    let profile =
      name.isEmpty
      ? nil : RoomProfile.seal(name: name, chainID: account.chainID, roomID: room.roomIDBytes, epoch: epoch, key: key)
    try await client.setProfile(room.roomID, member: account.chainID, profile: profile)
  }

  /// "Your Name for Sharing" changed: every room hears it.
  public func setDisplayName(_ name: String) {
    displayName = name
    for room in rooms.values {
      guard let account = account(of: room) else { continue }
      Task { try? await setProfile(room, account: account) }
    }
  }

  // MARK: Joining

  /// Joins with a link: membership with the invite's role, the mount in
  /// `account` (placed in `space`, for a folder or an easel), the records
  /// read.
  @discardableResult
  public func join(_ link: RoomInviteLink, account: Account, space: Space?) async throws -> RoomSync {
    let roomID = RoomInviteLink.b64(link.roomID)
    if let room = rooms[roomID] { return room }
    let client = client(account, server: link.serverURL)
    try await registerIdentity(account, client: client)
    let info = try await client.join(
      roomID, inviteID: link.inviteID, secret: link.inviteSecret, identityPub: account.identity.publicKey)
    var mount = RoomMount()
    mount.roomID = roomID
    mount.serverURL = link.serverURL
    mount.kind = link.kind.rawValue
    mount.role = info.role.rawValue
    mount.name = link.name
    mount.setKey(link.epochKey, epoch: link.epoch)
    if let space, link.kind != .space { mount.spaceUuid = space.id }
    return try await finishJoin(mount, info: info, account: account, client: client)
  }

  func finishJoin(_ base: RoomMount, info: RoomInfo, account: Account, client: RoomClient) async throws -> RoomSync {
    var mount = base
    for envelope in info.envelopes where mount.keyByEpoch[envelope.epoch] == nil {
      if let key = RoomCrypto.openEnvelope(
        identity: account.identity, roomID: mount.roomIDBytes ?? Data(), epoch: envelope.epoch, envelope: envelope.sealed)
      {
        mount.setKey(key, epoch: envelope.epoch)
      }
    }
    let room = start(mount, profile: account.profile)
    room.info = info
    try await room.pull(client, identity: account.identity)
    account.bridge?.putRoomMount(room.mount)
    for bridge in directory.bridges { bridge.roomsChanged() }
    try? await setProfile(room, account: account)
    revision += 1
    return room
  }

  /// Email invitations of an account (pending memberships).
  public func refreshInvitations(_ profile: String) async {
    guard let account = accounts[profile] else { return }
    let client = client(account)
    guard let list = try? await client.rooms() else { return }
    var out = invitations.filter { $0.account != profile }
    for summary in list where summary.pending && rooms[summary.id] == nil {
      guard let kind = RoomKind(rawValue: summary.kind) else { continue }
      var from = String(localized: "Someone", bundle: .module, comment: "Sharing: who sent an invitation, when their name is unknown.")
      if let info = try? await client.room(summary.id), let owner = info.member(info.owner) {
        from = owner.name.isEmpty ? (owner.email.isEmpty ? from : owner.email) : owner.name
      }
      out.append(Invitation(roomID: summary.id, kind: kind, from: from, account: profile))
    }
    invitations = out
  }

  /// Accepts an email invitation.
  @discardableResult
  public func accept(_ invitation: Invitation, space: Space?) async throws -> RoomSync {
    guard let account = accounts[invitation.account] else { throw RoomError.removed }
    let client = client(account)
    let info = try await client.join(invitation.roomID, identityPub: account.identity.publicKey)
    var mount = RoomMount()
    mount.roomID = invitation.roomID
    mount.serverURL = account.serverURL.absoluteString
    mount.kind = invitation.kind.rawValue
    mount.role = info.role.rawValue
    if let space, invitation.kind != .space { mount.spaceUuid = space.id }
    invitations.removeAll { $0.roomID == invitation.roomID }
    return try await finishJoin(mount, info: info, account: account, client: client)
  }

  public func decline(_ invitation: Invitation) async throws {
    guard let account = accounts[invitation.account] else { return }
    try await client(account).removeMember(invitation.roomID, member: account.chainID)
    invitations.removeAll { $0.roomID == invitation.roomID }
  }
}
