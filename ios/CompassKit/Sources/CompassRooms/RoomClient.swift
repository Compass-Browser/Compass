// Copyright 2026 The Compass Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import CompassCrypto
import Foundation

/// A member's role in a room.
public enum RoomRole: String, Sendable, Codable, CaseIterable {
  case owner, editor, viewer

  /// Whether this role writes records.
  public var canEdit: Bool { self != .viewer }
}

/// The rooms API (`<server>/compass/rooms`, docs/design/sync.md, "Rooms"):
/// every route, signed with one account's chain token. The server URL is
/// the sync server's, ending in /v2 (as a link's `s`).
///
/// Paths are built only from values checked against the server's own
/// grammar (room and invite IDs, chain IDs), then escaped anyway: nothing a
/// link or a server answer carries can add a path segment or a query.
public struct RoomClient: Sendable {
  public let serverURL: URL
  let authKey: Data
  /// The calling device's cache GUID (`client_id`): its own changes' events
  /// skip its stream, and a removed device is refused.
  let cacheGUID: String?
  let session: URLSession
  /// Seconds a long poll may wait (the server holds 30 at most).
  public static let maxWait = 30

  public init(serverURL: URL, authKey: Data, cacheGUID: String? = nil, session: URLSession? = nil) {
    self.serverURL = serverURL
    self.authKey = authKey
    self.cacheGUID = cacheGUID
    self.session = session ?? Self.defaultSession
  }

  /// Long polls hold for 30 s; give them room.
  static let defaultSession: URLSession = {
    let configuration = URLSessionConfiguration.default
    configuration.timeoutIntervalForRequest = 75
    configuration.requestCachePolicy = .reloadIgnoringLocalCacheData
    return URLSession(configuration: configuration)
  }()

  /// The account's chain ID, as the server writes it.
  public var chainID: String { Ed25519.publicKey(fromSeed: authKey).hexString(uppercase: false) }

  // MARK: Rooms

  /// `GET /rooms`: the caller's memberships, pending ones too.
  public func rooms() async throws -> [RoomSummary] {
    struct Answer: Decodable { var rooms: [RoomSummary] }
    return try await send("GET", ["compass", "rooms"], as: Answer.self).rooms
  }

  /// `POST /rooms`: a new room, the caller its owner. `id`: the room ID
  /// the client chose (16 random bytes), which `envelope` (epoch 1) is
  /// sealed for; one already taken is `RoomError.roomExists`.
  public func createRoom(id: Data, kind: RoomKind, identityPub: Data, envelope: Data, profile: Data? = nil)
    async throws -> RoomInfo
  {
    guard id.count == RoomCrypto.roomIDLength else { throw RoomError.badArgument("room ID") }
    var body: [String: Any] = [
      "id": id.base64URLEncodedString(padded: false), "kind": kind.rawValue,
      "identity_pub": identityPub.base64EncodedString(), "envelope": envelope.base64EncodedString(),
    ]
    if let profile { body["profile"] = profile.base64EncodedString() }
    return try await send("POST", ["compass", "rooms"], body: body, as: RoomInfo.self)
  }

  /// `GET /rooms/{room}`.
  public func room(_ id: String) async throws -> RoomInfo {
    try await send("GET", try roomPath(id), as: RoomInfo.self)
  }

  /// `DELETE /rooms/{room}`: Delete for Everyone (owner).
  public func deleteRoom(_ id: String) async throws {
    try await sendEmpty("DELETE", try roomPath(id))
  }

  /// `GET /rooms/{room}/changes`: records after `since`, waiting up to
  /// `wait` seconds for one when there's none.
  public func changes(_ id: String, since: Int64, limit: Int? = nil, wait: Int = 0) async throws -> RoomChanges {
    var query: [(String, String)] = [("since", String(max(0, since)))]
    if let limit { query.append(("limit", String(min(max(limit, 1), 1000)))) }
    if wait > 0 { query.append(("wait", String(min(wait, Self.maxWait)))) }
    return try await send(
      "GET", try roomPath(id) + ["changes"], query: query, as: RoomChanges.self,
      timeout: TimeInterval(wait + 45))
  }

  /// `POST /rooms/{room}/records`: upserts and deletes, all or nothing.
  /// A `base_seq` that doesn't match throws `RoomError.conflict` with the
  /// stored records; another epoch than the room's, `.staleEpoch`.
  public func write(_ id: String, records: [RoomWrite]) async throws -> RoomWriteResult {
    let body: [String: Any] = ["records": records.map(\.json)]
    return try await send("POST", try roomPath(id) + ["records"], body: body, as: RoomWriteResult.self)
  }

  // MARK: Invites

  /// `POST /rooms/{room}/invites`: a link invite. `expires`: when it stops
  /// working (nil: never); `uses`: how many joins (nil: unlimited).
  public func createInvite(_ id: String, role: RoomRole, secretHash: String, expires: Date? = nil, uses: Int? = nil)
    async throws -> RoomInvite
  {
    var body: [String: Any] = ["role": role.rawValue, "secret_hash": secretHash]
    if let expires { body["expires"] = Self.millis(expires) }
    if let uses { body["uses"] = uses }
    return try await send("POST", try roomPath(id) + ["invites"], body: body, as: RoomInvite.self)
  }

  /// `DELETE /rooms/{room}/invites/{invite}`.
  public func revokeInvite(_ id: String, invite: String) async throws {
    guard RoomInviteLink.isValidInviteID(invite) else { throw RoomError.badArgument("invite ID") }
    try await sendEmpty("DELETE", try roomPath(id) + ["invites", invite])
  }

  /// `POST /rooms/{room}/join`: with a link's invite and secret, or as a
  /// pending member (email invite) with neither.
  public func join(
    _ id: String, inviteID: String? = nil, secret: Data? = nil, identityPub: Data, profile: Data? = nil
  ) async throws -> RoomInfo {
    var body: [String: Any] = ["identity_pub": identityPub.base64EncodedString()]
    if let inviteID { body["invite_id"] = inviteID }
    if let secret { body["secret"] = secret.base64URLEncodedString(padded: false) }
    if let profile { body["profile"] = profile.base64EncodedString() }
    return try await send("POST", try roomPath(id) + ["join"], body: body, as: RoomInfo.self)
  }

  // MARK: Members

  /// `POST /rooms/{room}/members`: email invites (company sign-in servers).
  public func inviteMembers(_ id: String, _ invitees: [RoomInvitee]) async throws -> RoomInfo {
    let body: [String: Any] = ["members": invitees.map(\.json)]
    return try await send("POST", try roomPath(id) + ["members"], body: body, as: RoomInfo.self)
  }

  /// `PATCH /rooms/{room}/members/{client}` with a role (owner).
  public func setRole(_ id: String, member: String, role: RoomRole) async throws {
    try await sendEmpty("PATCH", try memberPath(id, member), body: ["role": role.rawValue])
  }

  /// `PATCH /rooms/{room}/members/{client}` with the caller's profile (nil
  /// clears it).
  public func setProfile(_ id: String, member: String, profile: Data?) async throws {
    try await sendEmpty(
      "PATCH", try memberPath(id, member), body: ["profile": profile.map { $0.base64EncodedString() } ?? NSNull()])
  }

  /// `DELETE /rooms/{room}/members/{client}`: remove (owner), or leave (the
  /// member itself).
  public func removeMember(_ id: String, member: String) async throws {
    try await sendEmpty("DELETE", try memberPath(id, member))
  }

  /// `POST /rooms/{room}/owner`: hands the room to another joined member.
  public func transferOwner(_ id: String, to member: String) async throws {
    guard Self.isChainID(member) else { throw RoomError.badArgument("client ID") }
    try await sendEmpty("POST", try roomPath(id) + ["owner"], body: ["client_id": member.lowercased()])
  }

  /// `POST /rooms/{room}/epochs`: the next epoch, one envelope per member.
  public func rotate(_ id: String, epoch: UInt32, envelopes: [String: Data]) async throws {
    let list = envelopes.keys.sorted().map { ["client_id": $0, "sealed": envelopes[$0]!.base64EncodedString()] }
    try await sendEmpty("POST", try roomPath(id) + ["epochs"], body: ["epoch": Int64(epoch), "envelopes": list])
  }

  /// `POST /rooms/{room}/envelopes`: keys of existing epochs for members.
  public func addEnvelopes(_ id: String, _ envelopes: [RoomEnvelopeFor]) async throws {
    let list = envelopes.map {
      ["client_id": $0.clientID, "epoch": Int64($0.epoch), "sealed": $0.sealed.base64EncodedString()] as [String: Any]
    }
    try await sendEmpty("POST", try roomPath(id) + ["envelopes"], body: ["envelopes": list])
  }

  // MARK: Identity and directory

  /// `GET /identity`: the caller's registered identity key, if any.
  public func identity() async throws -> Data? {
    struct Answer: Decodable { var identity_pub: Data? }
    return try await send("GET", ["compass", "identity"], as: Answer.self).identity_pub
  }

  /// `PUT /identity`.
  public func putIdentity(_ publicKey: Data) async throws {
    try await sendEmpty("PUT", ["compass", "identity"], body: ["identity_pub": publicKey.base64EncodedString()])
  }

  /// `GET /directory?email=`: that user's accounts (company sign-in).
  public func directory(email: String) async throws -> [DirectoryAccount] {
    struct Answer: Decodable { var accounts: [DirectoryAccount] }
    return try await send("GET", ["compass", "directory"], query: [("email", email)], as: Answer.self).accounts
  }

  /// `GET /info`'s sign-in: the provider's name when the server has company
  /// sign-in, nil when not.
  public func loginProvider() async throws -> String? {
    struct Login: Decodable { var type: String?; var name: String? }
    struct Answer: Decodable { var login: Login? }
    let answer = try await send("GET", ["compass", "info"], signed: false, as: Answer.self)
    guard let login = answer.login else { return nil }
    return login.name ?? "your company"
  }

  // MARK: Requests

  static let jsonDecoder: JSONDecoder = {
    let decoder = JSONDecoder()
    decoder.dataDecodingStrategy = .base64
    return decoder
  }()

  static func millis(_ date: Date) -> Int64 { Int64((date.timeIntervalSince1970 * 1000).rounded(.down)) }

  func roomPath(_ id: String) throws -> [String] {
    guard Self.isRoomID(id) else { throw RoomError.badArgument("room ID") }
    return ["compass", "rooms", id]
  }

  func memberPath(_ id: String, _ member: String) throws -> [String] {
    guard Self.isChainID(member) else { throw RoomError.badArgument("client ID") }
    return try roomPath(id) + ["members", member.lowercased()]
  }

  /// A room ID as the server makes them: canonical unpadded base64url of
  /// 16 bytes (22 characters).
  public static func isRoomID(_ id: String) -> Bool {
    id.utf8.count == 22 && RoomInviteLink.unb64(id)?.count == RoomCrypto.roomIDLength
  }

  /// A chain ID: 64 hex digits.
  public static func isChainID(_ id: String) -> Bool {
    id.utf8.count == 64 && id.utf8.allSatisfy { ($0 >= 48 && $0 <= 57) || ($0 >= 97 && $0 <= 102) || ($0 >= 65 && $0 <= 70) }
  }

  /// RFC 3986's unreserved characters, the only ones left as they are.
  static let unreserved = CharacterSet(
    charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~")

  static func escape(_ value: String) -> String {
    value.addingPercentEncoding(withAllowedCharacters: unreserved) ?? ""
  }

  /// `<server>/<segments…>?<query>`, each segment and value escaped.
  func url(_ segments: [String], query: [(String, String)]) -> URL {
    var c = URLComponents(url: serverURL, resolvingAgainstBaseURL: false)!
    var base = c.percentEncodedPath
    while base.hasSuffix("/") { base.removeLast() }
    c.percentEncodedPath = base + "/" + segments.map(Self.escape).joined(separator: "/")
    var items = query
    if let cacheGUID, !cacheGUID.isEmpty { items.append(("client_id", cacheGUID)) }
    c.percentEncodedQuery = items.isEmpty ? nil : items.map { Self.escape($0.0) + "=" + Self.escape($0.1) }.joined(separator: "&")
    c.fragment = nil
    return c.url!
  }

  func request(
    _ method: String, _ segments: [String], query: [(String, String)] = [], body: [String: Any]? = nil,
    signed: Bool = true, timeout: TimeInterval = 30
  ) throws -> URLRequest {
    var request = URLRequest(url: url(segments, query: query), timeoutInterval: timeout)
    request.httpMethod = method
    request.cachePolicy = .reloadIgnoringLocalCacheData
    request.setValue("application/json", forHTTPHeaderField: "Accept")
    if signed {
      request.setValue("Bearer " + Chain.accessToken(authKey: authKey, now: Date()), forHTTPHeaderField: "Authorization")
    }
    if let body {
      request.httpBody = try JSONSerialization.data(withJSONObject: body, options: [.sortedKeys])
      request.setValue("application/json", forHTTPHeaderField: "Content-Type")
    }
    return request
  }

  func send<T: Decodable>(
    _ method: String, _ segments: [String], query: [(String, String)] = [], body: [String: Any]? = nil,
    signed: Bool = true, as type: T.Type, timeout: TimeInterval = 30
  ) async throws -> T {
    let data = try await perform(
      request(method, segments, query: query, body: body, signed: signed, timeout: timeout))
    do {
      return try Self.jsonDecoder.decode(T.self, from: data)
    } catch {
      throw RoomError.badReply
    }
  }

  func sendEmpty(_ method: String, _ segments: [String], body: [String: Any]? = nil) async throws {
    _ = try await perform(request(method, segments, body: body))
  }

  func perform(_ request: URLRequest) async throws -> Data {
    let data: Data
    let response: URLResponse
    do {
      (data, response) = try await session.data(for: request)
    } catch {
      throw RoomError.network(error.localizedDescription)
    }
    guard let http = response as? HTTPURLResponse else { throw RoomError.badReply }
    guard (200..<300).contains(http.statusCode) else { throw RoomError(status: http, body: data) }
    return data
  }
}

// MARK: - Answers

/// One of `GET /rooms`.
public struct RoomSummary: Sendable, Decodable, Equatable {
  public var id: String
  public var kind: String
  public var owner: String
  public var role: RoomRole
  public var pending: Bool
  public var epoch: UInt32
  public var seq: Int64
}

/// A room as `GET /rooms/{room}` answers it.
public struct RoomInfo: Sendable, Decodable, Equatable {
  public var id: String
  public var kind: String
  public var owner: String
  public var created: Int64
  public var epoch: UInt32
  public var seq: Int64
  public var bytes: Int64
  /// The caller's.
  public var role: RoomRole
  public var pending: Bool
  public var members: [RoomMember]
  /// The caller's own, by epoch.
  public var envelopes: [RoomEnvelope]
  /// Only in the owner's answers.
  public var invites: [RoomInvite]?

  public func member(_ chainID: String) -> RoomMember? {
    members.first { $0.clientID.lowercased() == chainID.lowercased() }
  }
}

public struct RoomMember: Sendable, Decodable, Equatable, Identifiable {
  public var clientID: String
  public var role: RoomRole
  public var identityPub: Data
  public var profile: Data?
  /// From company sign-in; empty without.
  public var name: String
  public var email: String
  public var invited: Int64
  public var joined: Int64
  public var pending: Bool
  public var id: String { clientID }

  enum CodingKeys: String, CodingKey {
    case clientID = "client_id", role, identityPub = "identity_pub", profile, name, email, invited, joined, pending
  }

  public init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    clientID = try c.decode(String.self, forKey: .clientID)
    role = try c.decode(RoomRole.self, forKey: .role)
    identityPub = try c.decode(Data.self, forKey: .identityPub)
    profile = try c.decodeIfPresent(Data.self, forKey: .profile)
    name = try c.decodeIfPresent(String.self, forKey: .name) ?? ""
    email = try c.decodeIfPresent(String.self, forKey: .email) ?? ""
    invited = try c.decodeIfPresent(Int64.self, forKey: .invited) ?? 0
    joined = try c.decodeIfPresent(Int64.self, forKey: .joined) ?? 0
    pending = try c.decodeIfPresent(Bool.self, forKey: .pending) ?? false
  }

  public init(
    clientID: String, role: RoomRole, identityPub: Data, profile: Data? = nil, name: String = "", email: String = "",
    invited: Int64 = 0, joined: Int64 = 0, pending: Bool = false
  ) {
    self.clientID = clientID
    self.role = role
    self.identityPub = identityPub
    self.profile = profile
    self.name = name
    self.email = email
    self.invited = invited
    self.joined = joined
    self.pending = pending
  }
}

public struct RoomEnvelope: Sendable, Decodable, Equatable {
  public var epoch: UInt32
  public var sealed: Data

  public init(epoch: UInt32, sealed: Data) {
    self.epoch = epoch
    self.sealed = sealed
  }
}

/// An envelope for a member, to send.
public struct RoomEnvelopeFor: Sendable, Equatable {
  public var clientID: String
  public var epoch: UInt32
  public var sealed: Data
  public init(clientID: String, epoch: UInt32, sealed: Data) {
    self.clientID = clientID
    self.epoch = epoch
    self.sealed = sealed
  }
}

public struct RoomInvite: Sendable, Decodable, Equatable, Identifiable {
  public var id: String
  public var role: RoomRole
  public var created: Int64
  /// Unix ms; 0 = never.
  public var expires: Int64
  /// nil: unlimited.
  public var usesLeft: Int?
  public var revoked: Bool

  enum CodingKeys: String, CodingKey {
    case id, role, created, expires, usesLeft = "uses_left", revoked
  }
}

public struct RoomRecord: Sendable, Decodable, Equatable {
  public var id: String
  public var seq: Int64
  public var epoch: UInt32
  public var deleted: Bool
  public var author: String
  public var ciphertext: Data?

  public init(id: String, seq: Int64, epoch: UInt32, deleted: Bool, author: String = "", ciphertext: Data?) {
    self.id = id
    self.seq = seq
    self.epoch = epoch
    self.deleted = deleted
    self.author = author
    self.ciphertext = ciphertext
  }

  enum CodingKeys: String, CodingKey { case id, seq, epoch, deleted, author, ciphertext }

  public init(from decoder: Decoder) throws {
    let c = try decoder.container(keyedBy: CodingKeys.self)
    id = try c.decode(String.self, forKey: .id)
    seq = try c.decodeIfPresent(Int64.self, forKey: .seq) ?? 0
    epoch = try c.decodeIfPresent(UInt32.self, forKey: .epoch) ?? 0
    deleted = try c.decodeIfPresent(Bool.self, forKey: .deleted) ?? false
    author = try c.decodeIfPresent(String.self, forKey: .author) ?? ""
    ciphertext = try c.decodeIfPresent(Data.self, forKey: .ciphertext)
  }
}

public struct RoomChanges: Sendable, Decodable, Equatable {
  public var seq: Int64
  public var epoch: UInt32
  public var records: [RoomRecord]
  public var more: Bool
}

/// One record to write.
public struct RoomWrite: Sendable, Equatable {
  public var id: String
  public var epoch: UInt32
  public var ciphertext: Data?
  public var deleted: Bool
  /// The `seq` the write is based on (0: must not exist); nil: it wins.
  public var baseSeq: Int64?

  public init(id: String, epoch: UInt32, ciphertext: Data?, deleted: Bool = false, baseSeq: Int64? = nil) {
    self.id = id
    self.epoch = epoch
    self.ciphertext = ciphertext
    self.deleted = deleted
    self.baseSeq = baseSeq
  }

  var json: [String: Any] {
    var out: [String: Any] = ["id": id, "epoch": Int64(epoch)]
    if deleted { out["deleted"] = true } else if let ciphertext { out["ciphertext"] = ciphertext.base64EncodedString() }
    if let baseSeq { out["base_seq"] = baseSeq }
    return out
  }
}

public struct RoomWriteResult: Sendable, Decodable, Equatable {
  public struct Written: Sendable, Decodable, Equatable {
    public var id: String
    public var seq: Int64
  }
  public var seq: Int64
  public var records: [Written]
}

/// An account to invite by email (from the directory), with its envelopes.
public struct RoomInvitee: Sendable, Equatable {
  public var clientID: String
  public var role: RoomRole
  public var identityPub: Data
  public var envelopes: [RoomEnvelope]

  public init(clientID: String, role: RoomRole, identityPub: Data, envelopes: [RoomEnvelope]) {
    self.clientID = clientID
    self.role = role
    self.identityPub = identityPub
    self.envelopes = envelopes
  }

  var json: [String: Any] {
    [
      "client_id": clientID.lowercased(), "role": role.rawValue, "identity_pub": identityPub.base64EncodedString(),
      "envelopes": envelopes.map { ["epoch": Int64($0.epoch), "sealed": $0.sealed.base64EncodedString()] as [String: Any] },
    ]
  }
}

public struct DirectoryAccount: Sendable, Decodable, Equatable, Identifiable {
  public var clientID: String
  public var identityPub: Data
  public var name: String
  public var email: String
  public var id: String { clientID }

  enum CodingKeys: String, CodingKey { case clientID = "client_id", identityPub = "identity_pub", name, email }
}


// MARK: - Errors

public enum RoomError: Error, Equatable, Sendable, CustomStringConvertible {
  /// No reply (offline, DNS, TLS…).
  case network(String)
  case badReply
  /// A value that can't go in a request (checked before sending).
  case badArgument(String)
  /// A `base_seq` didn't match: the records as stored.
  case conflict([RoomRecord])
  /// The room's epoch moved: read it again.
  case staleEpoch
  /// The caller was removed from the room (or left).
  case removed
  /// No such room (deleted), or never a member.
  case notFound
  /// A rotation's envelopes don't match the members.
  case envelopesMismatch(missing: [String], unknown: [String])
  /// Email invite of an account that isn't signed in.
  case notSignedIn(String)
  /// Creating a room with an ID that's taken: choose another.
  case roomExists
  /// Any other answer: the status and the server's `error`.
  case refused(Int, String)

  init(status response: HTTPURLResponse, body: Data) {
    let json = (try? JSONSerialization.jsonObject(with: body)) as? [String: Any] ?? [:]
    let code = json["error"] as? String ?? ""
    switch (response.statusCode, code) {
    case (409, "conflict"):
      let current = (json["current"] as? [Any]).flatMap { try? JSONSerialization.data(withJSONObject: $0) }
        .flatMap { try? RoomClient.jsonDecoder.decode([RoomRecord].self, from: $0) }
      self = .conflict(current ?? [])
    case (409, "stale_epoch"): self = .staleEpoch
    case (409, "envelopes_mismatch"):
      self = .envelopesMismatch(missing: json["missing"] as? [String] ?? [], unknown: json["unknown"] as? [String] ?? [])
    case (409, "not_signed_in"): self = .notSignedIn(json["client_id"] as? String ?? "")
    case (409, "room_exists"): self = .roomExists
    case (403, "removed"): self = .removed
    case (404, "not_found"): self = .notFound
    default: self = .refused(response.statusCode, code)
    }
  }

  /// The server's `error` code, for the cases kept as `refused`.
  public var code: String {
    switch self {
    case .refused(_, let code): code
    case .conflict: "conflict"
    case .staleEpoch: "stale_epoch"
    case .removed: "removed"
    case .notFound: "not_found"
    case .envelopesMismatch: "envelopes_mismatch"
    case .notSignedIn: "not_signed_in"
    case .roomExists: "room_exists"
    default: ""
    }
  }

  /// Words for the user.
  public var description: String {
    switch self {
    case .network(let message): String(localized: "Can't reach the server (\(message)).", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
    case .badReply: String(localized: "The server's answer couldn't be read.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
    case .badArgument(let what): String(localized: "Invalid \(what).", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
    case .conflict: String(localized: "Someone else changed this at the same time.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
    case .staleEpoch: String(localized: "The room's key changed. Try again.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
    case .removed: String(localized: "You're no longer a member.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
    case .notFound: String(localized: "This shared item no longer exists.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
    case .envelopesMismatch: String(localized: "The members changed meanwhile. Try again.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
    case .notSignedIn: String(localized: "That person hasn't signed in to Compass on this server yet.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
    case .roomExists: String(localized: "Couldn't create the shared item. Try again.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
    case .refused(_, let code):
      switch code {
      case "invite_invalid": String(localized: "This invite link isn't valid.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "invite_expired": String(localized: "This invite link has expired.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "invite_used_up": String(localized: "This invite link has been used up.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "invite_revoked": String(localized: "This invite link was turned off.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "forbidden": String(localized: "Your role doesn't allow that.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "room_full": String(localized: "This shared item has the most members it can have.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "already_member": String(localized: "That person is already a member.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "chain_not_allowed": String(localized: "This server doesn't allow your account.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "login_required": String(localized: "Sign in to this server first.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "quota_exceeded": String(localized: "The owner's storage on the server is full.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "owner_must_transfer": String(localized: "The owner can't leave. Delete it for everyone instead.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "directory_disabled": String(localized: "Inviting by email needs a server with company sign-in.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      case "no_identity_key", "identity_mismatch": String(localized: "That account can't be invited yet. Try again later.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
      default:
        code.isEmpty
          ? String(localized: "The server refused.", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error.")
          : String(localized: "The server refused (\(code)).", bundle: .module, comment: "Sharing (shared spaces, folders and Easels): error. The argument is the server's error code.")
      }
    }
  }
}
